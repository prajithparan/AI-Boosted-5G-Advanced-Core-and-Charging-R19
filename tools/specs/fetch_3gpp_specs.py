#!/usr/bin/env python3
"""Fetch 3GPP specifications from the official archive and convert them to text.

Source: https://www.3gpp.org/ftp/Specs/archive/<series>_series/<spec>/ -- the 3GPP archive
itself, not a mirror. Each spec's directory is listed and the newest version for the requested
release is taken; 3GPP encodes the release as the first character of the version code
(f=15, g=16, h=17, i=18, j=19), so Release 19 is the highest j?? file.

What this records, per spec, in <out>/MANIFEST.tsv -- and why each column is there:

    spec        the TS number
    version     the 3GPP version code actually fetched, e.g. j10 == V19.1.0
    release     "19" when a Release-19 file existed, otherwise the release that WAS fetched
    note        empty, or "no Rel-19 version exists; latest is <ver>"

The last column is the honest one. A specification that was not touched in Release 19 has no
Release-19 file -- its most recent version simply remains in force -- and a manifest that silently
reported "Release 19" for it would be a fabrication. It is written down instead.

Text conversion is LibreOffice headless. It is the only .doc/.docx converter on this machine that
handles both the older .doc specs and the newer .docx ones, and it preserves table content, which
is where 3GPP puts most normative requirement lists.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import urllib.request
import zipfile

ARCHIVE = "https://www.3gpp.org/ftp/Specs/archive"
UA = {"User-Agent": "Mozilla/5.0 (spec-fetch; 5gc-r19 project)"}
RELEASE_LETTER = {15: "f", 16: "g", 17: "h", 18: "i", 19: "j", 20: "k"}


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def list_versions(series: str, spec: str) -> list[str]:
    html = fetch(f"{ARCHIVE}/{series}_series/{spec}/").decode("utf-8", "replace")
    stem = spec.replace(".", "")
    return sorted(set(re.findall(rf"{stem}-([a-z0-9]{{3}})\.zip", html)))


def pick(versions: list[str], release: int) -> tuple[str, int, str]:
    letter = RELEASE_LETTER[release]
    wanted = [v for v in versions if v[0] == letter]
    if wanted:
        return max(wanted), release, ""
    # No file for the requested release. Take the newest release that does exist and SAY SO.
    by_release = sorted(versions, key=lambda v: (v[0], v[1:]))
    latest = by_release[-1]
    rel = next((r for r, l in RELEASE_LETTER.items() if l == latest[0]), 0)
    return latest, rel, f"no Rel-{release} version exists; latest is {latest}"


def convert_to_text(doc: pathlib.Path, out_dir: pathlib.Path) -> pathlib.Path:
    subprocess.run(["libreoffice", "--headless", "--convert-to", "txt:Text", "--outdir",
                    str(out_dir), str(doc)], check=True, capture_output=True, timeout=600)
    txt = out_dir / (doc.stem + ".txt")
    if not txt.exists():
        raise RuntimeError(f"conversion produced no text for {doc}")
    return txt


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spec", action="append", required=True, help="e.g. 33.501; repeatable")
    ap.add_argument("--release", type=int, default=19)
    ap.add_argument("--out", default="specs/3gpp")
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = out / "MANIFEST.tsv"
    # Merge into the existing manifest rather than overwrite it (2026-09-14): every earlier run
    # replaced the file wholesale, so fetching 29.500 silently dropped the 23.288 row, which had
    # itself dropped the 33-series rows. A manifest is only an inventory if it keeps every entry.
    existing: dict[str, str] = {}
    if manifest.exists():
        for line in manifest.read_text().splitlines()[1:]:
            if line.strip():
                existing[line.split("\t", 1)[0]] = line
    rows = ["spec\tversion\trelease\tnote"]

    for spec in args.spec:
        series = spec.split(".")[0]
        try:
            versions = list_versions(series, spec)
            if not versions:
                rows.append(f"{spec}\t-\t-\tno versions listed in the archive")
                print(f"{spec}: NOTHING LISTED", file=sys.stderr)
                continue
            version, rel, note = pick(versions, args.release)
            stem = spec.replace(".", "")
            zip_path = out / f"{stem}-{version}.zip"
            if not zip_path.exists():
                zip_path.write_bytes(fetch(f"{ARCHIVE}/{series}_series/{spec}/{stem}-{version}.zip"))
            with zipfile.ZipFile(zip_path) as z:
                docs = [n for n in z.namelist() if n.lower().endswith((".docx", ".doc"))]
                if not docs:
                    raise RuntimeError(f"{zip_path} holds no .doc/.docx: {z.namelist()}")
                # The main document is the one named like the zip; annexes/cover pages are extras.
                main_doc = sorted(docs, key=lambda n: (stem not in n.lower(), len(n)))[0]
                z.extract(main_doc, out)
            txt = convert_to_text(out / main_doc, out)
            final = out / f"TS_{spec}_{version}.txt"
            txt.rename(final)
            rows.append(f"{spec}\t{version}\t{rel}\t{note}")
            print(f"{spec}: {version} (Rel-{rel}) {note} -> {final.name}", file=sys.stderr)
        except Exception as exc:  # one spec failing must not lose the others
            rows.append(f"{spec}\t-\t-\tFAILED: {exc}")
            print(f"{spec}: FAILED {exc}", file=sys.stderr)

    for row in rows[1:]:
        existing[row.split("\t", 1)[0]] = row
    rows = rows[:1] + [existing[k] for k in sorted(existing)]
    manifest.write_text("\n".join(rows) + "\n")
    print(f"manifest: {manifest}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
