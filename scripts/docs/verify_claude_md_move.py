#!/usr/bin/env python3
"""Lossless-move check for CLAUDE.md: every sentence of the original (default `git show main:CLAUDE.md`)
must appear verbatim (whitespace-normalised) in the new CLAUDE.md or a file under docs/project-context/.
usage: verify_claude_md_move.py [--orig-ref REF]"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def norm(t):
    return re.sub(r"\s+", " ", t).strip()


def body(t):
    # a heading line or a list-item start ends the previous sentence: mark it with a separator token
    return "\n".join("§§" if l.startswith("#") else ("§§" + l if re.match(r"(- |\d+\. )", l) else l) for l in t.split("\n"))


ap = argparse.ArgumentParser()
ap.add_argument("--orig-ref", default="main")
a = ap.parse_args()
orig = subprocess.run(["git", "show", f"{a.orig_ref}:CLAUDE.md"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
corpus_raw = (ROOT / "CLAUDE.md").read_text(encoding="utf-8") + "\n".join(
    p.read_text(encoding="utf-8") for p in sorted((ROOT / "docs/project-context").glob("*.md")))
hay = norm(body(corpus_raw))
missing = [s for s in re.split(r"(?<=[.!?])\s+|\s*§§\s*", norm(body(orig))) if s and s not in hay]
heads = [l.lstrip("# ").strip() for l in orig.split("\n") if l.startswith("#")]
hay_all = norm(corpus_raw)
mh = [h for h in heads if h not in hay_all]
total = len(re.split(r"(?<=[.!?])\s+|\s*§§\s*", norm(body(orig))))
print(f"sentences checked: {total}; missing: {len(missing)}; headings not found: {len(mh)}")
for s in missing[:10]:
    print("MISSING:", s[:140])
for h in mh:
    print("HEADING NOT FOUND:", h)
sys.exit(1 if missing or mh else 0)
