#!/usr/bin/env python3
"""Sync a freshly generated staging directory into the real generated directory, preserving the
timestamps of files whose content did not change.

Why this exists (2026-09-14): libs/sbi-generated/CMakeLists.txt regenerates every DTO at every
CMake configure and wiped the output directory first (a correct fix for the stale-merged-header
redefinition bug it documents). The cost was that ANY reconfigure -- a one-line change to any
NF's CMakeLists.txt -- gave all ~130 generated translation units new timestamps, and Ninja then
recompiled sbi_generated and every NF and test that links it: 30-60 minutes for a build whose
generated output was byte-identical. This keeps the clean-slate semantics (files that no longer
exist in the new output are deleted, so a stale standalone header can never coexist with its
merged replacement) while only touching files whose bytes actually changed.
"""
import filecmp
import shutil
import sys
from pathlib import Path

KEEP = {".stamp"}  # the build-system marker lives in the destination and is not generated


def main(staging: Path, dest: Path) -> int:
    if not staging.is_dir():
        print(f"sync_generated: staging dir {staging} missing", file=sys.stderr)
        return 1
    dest.mkdir(parents=True, exist_ok=True)
    new_names = {p.name for p in staging.iterdir() if p.is_file()}
    removed = 0
    for old in dest.iterdir():
        if old.is_file() and old.name not in new_names and old.name not in KEEP:
            old.unlink()
            removed += 1
    copied = 0
    for src in staging.iterdir():
        if not src.is_file():
            continue
        dst = dest / src.name
        if dst.exists() and filecmp.cmp(src, dst, shallow=False):
            continue
        shutil.copy2(src, dst)
        copied += 1
    shutil.rmtree(staging)
    print(f"sync_generated: {copied} changed, {removed} stale removed, "
          f"{len(new_names) - copied} unchanged (timestamps kept)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: sync_generated.py <staging-dir> <dest-dir>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(Path(sys.argv[1]), Path(sys.argv[2])))
