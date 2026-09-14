#!/usr/bin/env python3
"""Fail if README.md's embedded architecture diagram differs from docs/diagrams/architecture.mmd.

Two copies exist on purpose -- README is the publish surface and GitHub cannot include a file into
a fenced block -- so the only defence against them drifting apart is a check. Run from CI's lint
job and locally before committing either file.
"""
import pathlib, re, sys
root = pathlib.Path(__file__).resolve().parents[2]
src = (root / "docs/diagrams/architecture.mmd").read_text()
readme = (root / "README.md").read_text()
m = re.search(r"```mermaid\n(flowchart TB\n.*?)```", readme, re.S)
if not m:
    print("README.md has no embedded architecture flowchart"); sys.exit(1)
if m.group(1) != src:
    print("README.md's architecture diagram differs from docs/diagrams/architecture.mmd -- "
          "edit the .mmd and paste it into README, or the other way round, but make them equal")
    sys.exit(1)
print("architecture diagram: README and docs/diagrams/architecture.mmd are identical")
