#!/usr/bin/env python3
"""Per-NF R19 path coverage: every path in the NF's OpenAPI YAML against the compiled binary.

Method of docs/NF_API_COVERAGE_AUDIT.md (ADR-0252), made reproducible: route registration builds
paths from API-root constants and path-pattern variables, so source regex cannot see the real
routes; the string table of the built binary can. A YAML path counts as routed when its shape (placeholder
names ignored) ends some path-like string in the binary. Never starts a process and opens no port.

usage: nf_api_coverage.py [--build-dir build] [--specs specs/5G_APIs-REL-19] [--list NF...] [--json]
Spec files are assigned to an NF by name: TS<nnnnn>_N<nf>_<service>.yaml -> <nf>.

Limits (read before quoting a number): a path that the code assembles at run time from pieces is a
false "unrouted"; a path string that exists only as a log or comment string is a false "routed".
The 2026-09-02 audit had the same limits. Counts are paths, not operations (path x method)."""
import argparse
import json
import mmap
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
PLACEHOLDER = re.compile(rb"\{[^{}/]*\}")
PATHLIKE = re.compile(rb"/[A-Za-z0-9_\-{}/.:]{2,}")
# directory name under nfs/ -> prefix in the spec file name (Nxxx), where it is not simply N<dir>
SPEC_PREFIX = {"eir": "N5g-eir"}


def spec_files(specs, nf):
    prefix = SPEC_PREFIX.get(nf, f"N{nf}")
    return sorted(p for p in specs.glob("TS*_*.yaml") if re.fullmatch(rf"TS\d+_{re.escape(prefix)}_.+\.yaml", p.name))


def yaml_paths(path):
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    return sorted((doc or {}).get("paths", {}) or {})


def normalised(path):
    """Placeholder names differ between the YAML and the code ({ueId} vs {supi}); the shape does not."""
    return PLACEHOLDER.sub(b"{}", path)


def binary_shapes(binary):
    """One pass over the binary: its path-like strings, placeholders normalised, one per line (+ a final
    newline). A path counts as routed when some string ENDS with it -- not merely contains it: `/pdu-sessions`
    must not count because `/pdu-sessions/{}/deliver` is routed, but `/nf-instances` must count when the
    compiler stored it only as the tail of `/nnrf-nfm/v1/nf-instances` (string-tail merging)."""
    with open(binary, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        return b"\n".join({normalised(m.group(0)) for m in PATHLIKE.finditer(mm)}) + b"\n"


def audit(build, specs, nf):
    binary = build / "nfs" / nf / nf
    files = spec_files(specs, nf)
    if not binary.is_file() or not files:
        return None
    blob = binary_shapes(binary)
    unrouted, total = [], 0
    for sf in files:
        for p in yaml_paths(sf):
            total += 1
            if blob.find(normalised(p.encode()) + b"\n") < 0:
                unrouted.append((sf.name, p))
    return {"nf": nf, "specs": len(files), "paths": total, "unrouted": unrouted}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=str(ROOT / "build"))
    ap.add_argument("--specs", default=str(ROOT / "specs" / "5G_APIs-REL-19"))
    ap.add_argument("--list", nargs="*", help="NF directory names (default: every nfs/* with a spec match)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    build, specs = Path(a.build_dir), Path(a.specs)
    nfs = a.list or sorted(p.name for p in (ROOT / "nfs").iterdir() if p.is_dir())
    rows, skipped = [], []
    for nf in nfs:
        r = audit(build, specs, nf)
        (rows if r else skipped).append(r or nf)
    if a.json:
        json.dump(rows, sys.stdout, indent=1)
        return
    print(f"{'NF':<8} {'specs':>5} {'paths':>6} {'unrouted':>9}")
    for r in rows:
        print(f"{r['nf']:<8} {r['specs']:>5} {r['paths']:>6} {len(r['unrouted']):>9}")
    print(f"TOTAL    {sum(r['specs'] for r in rows):>5} {sum(r['paths'] for r in rows):>6} {sum(len(r['unrouted']) for r in rows):>9}")
    for r in rows:
        for spec, p in r["unrouted"]:
            print(f"  {r['nf']}: {p}   ({spec})")
    if skipped:
        print("no binary or no spec match (not audited):", " ".join(skipped))


if __name__ == "__main__":
    main()
