#!/usr/bin/env python3
"""sage-import, stage 1 (F2): ingest and fingerprint an operator's licensed SAGE package (ADR-0479).

This tool contains NO algorithm knowledge and NO restricted content. It does not parse the
documents (their layout has not been seen; extractors are stage 2 and are not written). It only:
  ingest : read <vault>/package.manifest.json (written by the operator: role -> file, expected
           document version), fingerprint every listed file (size + sha256) and write
           <vault>/package.lock.json. Refuses to overwrite an existing lock whose fingerprints
           differ unless --accept-new-package.
  verify : re-hash the vault and compare with the lock; exit 1 on any difference.
The vault must be outside git or ignored by it (checked with `git check-ignore`).
Output never includes file contents.

package.manifest.json (operator-written):
  {"package_id": "operator-chosen-id",
   "files": {"doc1": {"path": "UEA2_UIA2_doc1.pdf", "expected_version": "1.1"}, ...}}
Roles are free text; this tool does not interpret them.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

MANIFEST = "package.manifest.json"
LOCK = "package.lock.json"
SCHEMA = 1


class ImportError_(Exception):
    pass


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def vault_is_safe(vault: Path) -> bool:
    """True if the vault is outside any git work tree, or git ignores it."""
    probe = vault if vault.exists() else vault.parent
    inside = subprocess.run(["git", "-C", str(probe), "rev-parse", "--is-inside-work-tree"],
                            capture_output=True, text=True)
    if inside.returncode != 0 or inside.stdout.strip() != "true":
        return True
    ignored = subprocess.run(["git", "-C", str(probe), "check-ignore", "-q", str(vault.resolve())],
                             capture_output=True)
    return ignored.returncode == 0


def fingerprint(vault: Path) -> dict:
    mf_path = vault / MANIFEST
    if not mf_path.is_file():
        raise ImportError_(f"{mf_path} missing: write the package manifest first (see --help)")
    mf = json.loads(mf_path.read_text())
    if not isinstance(mf.get("package_id"), str) or not mf["package_id"]:
        raise ImportError_("manifest needs a non-empty string package_id")
    files = mf.get("files")
    if not isinstance(files, dict) or not files:
        raise ImportError_("manifest needs a non-empty files map")
    out = {}
    for role, ent in sorted(files.items()):
        rel = ent.get("path") if isinstance(ent, dict) else None
        if not isinstance(rel, str) or rel.startswith("/") or ".." in Path(rel).parts:
            raise ImportError_(f"role {role}: path must be relative and stay inside the vault")
        f = vault / rel
        if not f.is_file():
            raise ImportError_(f"role {role}: file {rel} not found")
        out[role] = {"path": rel, "size": f.stat().st_size, "sha256": sha256_file(f),
                     "expected_version": ent.get("expected_version", "")}
    return {"schema": SCHEMA, "package_id": mf["package_id"], "files": out}


def cmd_ingest(vault: Path, accept_new: bool) -> int:
    if not vault_is_safe(vault):
        print(f"REFUSED: {vault} is inside a git work tree and not ignored", file=sys.stderr)
        return 2
    fp = fingerprint(vault)
    lock = vault / LOCK
    if lock.exists() and not accept_new:
        old = json.loads(lock.read_text())
        if old != fp:
            print("REFUSED: package differs from the existing lock (use --accept-new-package "
                  "after confirming this is intended)", file=sys.stderr)
            return 1
    lock.write_text(json.dumps(fp, indent=2, sort_keys=True) + "\n")
    print(f"ingested package_id={fp['package_id']} files={len(fp['files'])} lock={lock}")
    return 0


def cmd_verify(vault: Path) -> int:
    lock = vault / LOCK
    if not lock.is_file():
        print("no lock: run ingest first", file=sys.stderr)
        return 1
    if json.loads(lock.read_text()) != fingerprint(vault):
        print("MISMATCH: vault differs from package.lock.json", file=sys.stderr)
        return 1
    print("verify OK")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["ingest", "verify"])
    ap.add_argument("vault", type=Path)
    ap.add_argument("--accept-new-package", action="store_true")
    a = ap.parse_args(argv[1:])
    try:
        return cmd_ingest(a.vault, a.accept_new_package) if a.command == "ingest" else cmd_verify(a.vault)
    except ImportError_ as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
