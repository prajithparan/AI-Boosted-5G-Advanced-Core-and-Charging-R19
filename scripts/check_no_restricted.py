#!/usr/bin/env python3
"""Fails if licence-restricted algorithm material is tracked in git (ADR-0479).

Checks every tracked file (git ls-files) for:
  1. a path under a forbidden vault/generated location or with a forbidden extension;
  2. a sha256 listed in snow3g/restricted-fingerprints.txt (operators append the fingerprints of
     their own package files; the file ships empty because this repo holds no such material).
Usage: check_no_restricted.py [repo_root]   (exit 1 on any hit; prints paths only, never content)
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
from pathlib import Path

FORBIDDEN_PREFIXES = ("snow3g/vault/", "snow3g/generated/", "snow3g/sage-package/")
FORBIDDEN_SUFFIXES = (".sage",)
FINGERPRINTS = "snow3g/restricted-fingerprints.txt"


def tracked(root: Path) -> list[str]:
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True)
    return [p for p in out.stdout.decode().split("\0") if p]


def load_fingerprints(root: Path) -> set[str]:
    p = root / FINGERPRINTS
    if not p.exists():
        return set()
    return {ln.split("#")[0].strip().lower() for ln in p.read_text().splitlines() if ln.split("#")[0].strip()}


def check(root: Path, files: list[str]) -> list[str]:
    bad: list[str] = []
    fps = load_fingerprints(root)
    for rel in files:
        if rel.startswith(FORBIDDEN_PREFIXES) or rel.endswith(FORBIDDEN_SUFFIXES):
            bad.append(f"{rel}: forbidden location/extension")
            continue
        if fps:
            f = root / rel
            if f.is_file() and hashlib.sha256(f.read_bytes()).hexdigest() in fps:
                bad.append(f"{rel}: matches a restricted-package fingerprint")
    return bad


def main(argv: list[str]) -> int:
    root = Path(argv[1]).resolve() if len(argv) > 1 else Path(__file__).resolve().parent.parent
    bad = check(root, tracked(root))
    for b in bad:
        print("RESTRICTED:", b)
    print(f"check_no_restricted: {'FAIL' if bad else 'OK'} ({len(bad)} hit(s))")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
