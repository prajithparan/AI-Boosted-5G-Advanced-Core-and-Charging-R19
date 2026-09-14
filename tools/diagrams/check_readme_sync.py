#!/usr/bin/env python3
"""Keep the README architecture diagram honest. Runs in CI's lint job and locally.

Three checks, each a way the diagram could drift from the code (ADR-0356, ADR-0361):

1. The committed SVGs are byte-identical to what render_architecture.py produces from
   docs/diagrams/architecture.json -- so a JSON edit without a re-render, or a hand edit of the
   SVG, fails the build. This is what makes a rendered image acceptable where ADR-0356 had
   rejected one: the render step is a check, not something to remember.
2. README.md embeds exactly those two files (light + dark) -- the README is the publish surface.
3. Every tile drawn solid ("built") names a `source` -- a directory or file in this repository,
   or a service in deploy/docker/docker-compose.yml -- and that source exists. A solid tile for
   something that is not in the tree is the overclaim CLAUDE.md forbids; this makes it a build
   failure rather than a review finding.
"""
import json
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[2]
failures: list[str] = []

# 1. rendered output is current
data = json.loads((root / "docs/diagrams/architecture.json").read_text())
sys.path.insert(0, str(root / "tools/diagrams"))
import render_architecture  # noqa: E402

for theme, name in (("light", "architecture.svg"), ("dark", "architecture-dark.svg")):
    committed = root / "docs/diagrams" / name
    if not committed.exists():
        failures.append(f"{name} is missing -- run tools/diagrams/render_architecture.py")
    elif committed.read_text() != render_architecture.render(data, theme):
        failures.append(f"{name} is stale against architecture.json -- run tools/diagrams/render_architecture.py")

# 2. README embeds them
readme = (root / "README.md").read_text()
for name in ("docs/diagrams/architecture.svg", "docs/diagrams/architecture-dark.svg"):
    if name not in readme:
        failures.append(f"README.md does not embed {name}")
if "```mermaid\nflowchart TB" in readme:
    failures.append("README.md still carries the old Mermaid architecture block (ADR-0361 replaced it)")

# 3. every built tile points at something that exists
compose = (root / "deploy/docker/docker-compose.yml").read_text()
services = set(re.findall(r"^  ([a-z][a-z0-9-]*):\s*$", compose, re.M))
for n in data["nodes"]:
    if n.get("planned"):
        continue
    src = n.get("source")
    if not src:
        failures.append(f"tile {n['id']} is drawn solid but names no source")
        continue
    if src.startswith("compose:"):
        if src[8:] not in services:
            failures.append(f"tile {n['id']}: compose service '{src[8:]}' not in docker-compose.yml")
    elif not (root / src).exists():
        failures.append(f"tile {n['id']}: source '{src}' does not exist")

if failures:
    print("\n".join("architecture diagram: " + f for f in failures))
    sys.exit(1)
built = sum(1 for n in data["nodes"] if not n.get("planned"))
planned = len(data["nodes"]) - built
print(f"architecture diagram: SVGs current, README embeds them, {built} built tiles resolve to sources, {planned} planned")
