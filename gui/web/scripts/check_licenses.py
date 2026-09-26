#!/usr/bin/env python3
"""P1 (OSI-only) gate for the GUI's npm dependency tree (ADR-0420).

Reads every package in gui/web/package-lock.json -- the full transitive tree, dev and runtime --
and fails if any declares a license outside the allow-list below (all OSI-approved). A package
with no license field fails too: unknown is not approved. Usage: check_licenses.py <package-lock>
"""

from __future__ import annotations

import json
import sys
from collections import Counter

# OSI-approved licenses actually present today. Extend only after checking the OSI list.
ALLOWED = {"MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "ISC", "MPL-2.0", "0BSD"}


def main(argv: list[str]) -> int:
    lock = json.load(open(argv[1]))
    bad, seen = [], Counter()
    for path, meta in lock.get("packages", {}).items():
        if not path:  # the root project itself
            continue
        name = path.rsplit("node_modules/", 1)[-1]
        lic = meta.get("license", "UNKNOWN")
        seen[lic] += 1
        if lic not in ALLOWED:
            bad.append(f"{name}: {lic}")
    for lic, n in sorted(seen.items()):
        print(f"{lic}: {n}")
    if bad:
        print("NOT OSI-allow-listed:\n  " + "\n  ".join(bad), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
