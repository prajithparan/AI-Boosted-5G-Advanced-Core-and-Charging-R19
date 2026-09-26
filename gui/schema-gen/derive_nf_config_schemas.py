#!/usr/bin/env python3
"""Derive one JSON Schema per NF / BSS service from its real static configuration (ADR-0425).

Source of truth, per component: config/<name>.json (the keys and value types an operator can set
today) plus the component's own C++ source (which keys it reads, how, and with which env
override). Nothing is typed by hand:

  * every property comes from a key present in config/<name>.json -- the schema never adds one;
  * `type` is inferred from the current value (int -> integer, float -> number, ...), nested
    objects/arrays recursively; nested objects are closed (additionalProperties: false);
  * `required` = top-level keys the source reads with nf_config::require<T>(config, "key", ...),
    which exits the process when the key is missing (no in-source default exists, ADR-0077);
  * `x-env-override` = the env var named in that require<>() call;
  * `x-cpp-type` = the T of require<T>, when present;
  * `x-sensitivity: credential` for keys whose name or value marks a credential (see
    is_credential) -- the GUI masks these and the BFF never audits them in clear;
  * `x-review` lists what the derivation could NOT establish: keys in the file that no source
    line visibly reads, and nested objects whose map-vs-struct meaning is unverified. Those are
    questions for review, not guesses.

Usage:
  derive_nf_config_schemas.py <repo_root> <out_dir>            write <out_dir>/<name>.schema.json
  derive_nf_config_schemas.py <repo_root> <out_dir> --check    exit 1 if any output is stale
  derive_nf_config_schemas.py <repo_root> <out_dir> --matrix   print the coverage matrix (markdown)
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

SEARCH_ROOTS = ["nfs", "bss", "tools"]
# Components whose directory name differs from the config file's name.
DIR_OVERRIDES = {"oam-gui-bff": ["gui/bff"]}
# Shared readers: keys read by libraries on the NF's behalf (sbi_core::read_tps_limit).
SHARED_SOURCES = ["libs/sbi-core/src/rate_limit.cpp", "libs/sbi-core/include/sbi_core/rate_limit.hpp"]

REQUIRE_RE = re.compile(
    r'require<\s*([^>]+?)\s*>\(\s*\w+\s*,\s*"([A-Za-z0-9_]+)"\s*(?:,\s*"([A-Z0-9_]+)")?')
READ_RE = re.compile(r'\.(?:value|at|contains|find|count)\(\s*"([A-Za-z0-9_]+)"|\[\s*"([A-Za-z0-9_]+)"\s*\]')
CRED_NAME_RE = re.compile(r"(password|passwd|secret|token|database_url|_dsn)$|^(password|secret)")


def source_dirs(repo: Path, name: str) -> list[Path]:
    if name in DIR_OVERRIDES:
        return [repo / d for d in DIR_OVERRIDES[name]]
    return [repo / r / name for r in SEARCH_ROOTS if (repo / r / name).is_dir()]


def scan(repo: Path, name: str):
    required: dict[str, dict] = {}
    read: set[str] = set()
    files = []
    for d in source_dirs(repo, name):
        files += [p for p in d.rglob("*") if p.suffix in (".cpp", ".hpp", ".h")]
    files += [repo / s for s in SHARED_SOURCES if (repo / s).exists()]
    for f in sorted(files):
        text = f.read_text(errors="replace")
        for cpp_type, key, env in REQUIRE_RE.findall(text):
            required[key] = {"cpp": cpp_type.strip(), "env": env or None}
            read.add(key)
        for a, b in READ_RE.findall(text):
            read.add(a or b)
    return required, read, files


def is_credential(key: str, value) -> bool:
    if CRED_NAME_RE.search(key):
        return True
    # A URL carrying user:password@ is a credential whatever the key is called.
    return isinstance(value, str) and re.match(r"^[a-z][a-z0-9+.-]*://[^/@\s]+:[^/@\s]+@", value) is not None


def value_schema(value, path: str, review: list[str]) -> dict:
    if isinstance(value, bool):
        return {"type": "boolean"}
    if isinstance(value, int):
        return {"type": "integer"}
    if isinstance(value, float):
        return {"type": "number"}
    if isinstance(value, str):
        return {"type": "string"}
    if value is None:
        review.append(f"{path}: null in the file, type unknown")
        return {}
    if isinstance(value, list):
        if not value:
            review.append(f"{path}: empty array, item type unknown")
            return {"type": "array"}
        items = [value_schema(v, path + "[]", []) for v in value]
        if all(isinstance(v, dict) for v in value):
            merged: dict = {}
            for v in value:
                merged.update(v)
            return {"type": "array", "items": value_schema(merged, path + "[]", review)}
        return {"type": "array", "items": items[0]}
    if isinstance(value, dict):
        review.append(f"{path}: nested object shape inferred from current values "
                      "(map-vs-struct meaning unverified; new keys refused until reviewed)")
        return {
            "type": "object",
            "additionalProperties": False,
            "properties": {k: value_schema(v, f"{path}.{k}", review) for k, v in value.items()},
        }
    raise SystemExit(f"{path}: unsupported JSON value {value!r}")


def derive_one(repo: Path, cfg: Path) -> tuple[dict, dict]:
    name = cfg.stem
    doc = json.loads(cfg.read_text())
    required, read, files = scan(repo, name)
    review: list[str] = []
    props = {}
    for key, value in doc.items():
        s = value_schema(value, key, review)
        if key in required:
            if required[key]["env"]:
                s["x-env-override"] = required[key]["env"]
            s["x-cpp-type"] = required[key]["cpp"]
        if is_credential(key, value):
            s["x-sensitivity"] = "credential"
        if key not in read:
            review.append(f"{key}: present in the file but no source line visibly reads it")
        props[key] = s
    missing = sorted(k for k in required if k not in doc)
    for k in missing:
        review.append(f"{k}: read with require<> but absent from the file (env-only?)")
    schema = {
        "$schema": "http://json-schema.org/draft-07/schema#",
        "$comment": ("GENERATED by gui/schema-gen/derive_nf_config_schemas.py from "
                     f"config/{name}.json + its source (ADR-0425). Do not edit; regenerate."),
        "title": name,
        "type": "object",
        "additionalProperties": False,
        "properties": props,
        "required": sorted(k for k in required if k in doc),
        "x-config-file": f"config/{name}.json",
        "x-source": sorted({str(d.relative_to(repo)) for d in source_dirs(repo, name)}),
        "x-apply": "restart",  # no component re-reads its config file at runtime (ADR-0425)
        "x-review": review,
    }
    info = {"name": name, "keys": len(doc), "required": len(schema["required"]),
            "credentials": sum(1 for p in props.values() if p.get("x-sensitivity")),
            "review": len(review), "sources": schema["x-source"], "files": len(files)}
    return schema, info


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    repo, out = Path(argv[1]), Path(argv[2])
    mode = argv[3] if len(argv) > 3 else ""
    results = [derive_one(repo, c) for c in sorted((repo / "config").glob("*.json"))]
    if mode == "--matrix":
        print("| Component | Config file | Keys | Required (require<>) | Credentials | Review items | Source |")
        print("|---|---|---|---|---|---|---|")
        for _, i in results:
            print(f"| {i['name']} | config/{i['name']}.json | {i['keys']} | {i['required']} | "
                  f"{i['credentials']} | {i['review']} | {', '.join(i['sources']) or '(none found)'} |")
        return 0
    stale = []
    for schema, info in results:
        rendered = json.dumps(schema, indent=2) + "\n"
        target = out / f"{info['name']}.schema.json"
        if mode == "--check":
            if not target.exists() or target.read_text() != rendered:
                stale.append(str(target))
        else:
            out.mkdir(parents=True, exist_ok=True)
            target.write_text(rendered)
    if mode == "--check":
        extra = sorted({p.name for p in out.glob("*.schema.json")} -
                       {f"{i['name']}.schema.json" for _, i in results})
        if stale or extra:
            print("stale NF config schemas (regenerate):", *stale, *extra, sep="\n  ", file=sys.stderr)
            return 1
        print(f"{len(results)} NF config schemas up to date")
        return 0
    print(f"wrote {len(results)} schemas to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
