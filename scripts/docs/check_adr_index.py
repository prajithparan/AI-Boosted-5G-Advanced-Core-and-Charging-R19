#!/usr/bin/env python3
"""Fail on ADR index drift or duplicate numbers; warn on numbering gaps (unassigned numbers are legitimate).
Checks: files on disk == _order.json == DECISIONS.md lines == INDEX_DETAIL.md rows; the original sections
still reassemble to the recorded sha256; the index preamble equals _other/_preamble.md."""
import re
import sys
from adr_lib import *


def main():
    errs, warns = [], []
    order = load_order()
    listed = order["original_files"] + order["appended"]
    on_disk = {p.name for p in adr_files()} | {f"_other/{p.name}" for p in OTHER.glob("*.md") if p.name != "_preamble.md"}
    if set(listed) != on_disk:
        errs.append(f"files vs _order.json: only on disk {sorted(on_disk - set(listed))[:5]}, only in manifest {sorted(set(listed) - on_disk)[:5]}")
    nums = [adr_number(p.name) for p in adr_files()]
    dup = sorted({n for n in nums if nums.count(n) > 1 and not any(f"ADR-{n:04d}-dup" in p.name for p in adr_files())})
    if dup:
        errs.append(f"duplicate ADR numbers without -dupN suffix: {dup}")
    idx = read(DECISIONS)
    linked = set(re.findall(r"\]\(decisions/((?:_other/)?[^)]+\.md)\)", idx))
    if linked != set(listed):
        errs.append(f"DECISIONS.md links vs manifest: missing {sorted(set(listed) - linked)[:5]}, extra {sorted(linked - set(listed))[:5]}")
    rows = set(re.findall(r"\[(ADR-[^\]]+\.md)\]\(", read(DETAIL)))
    if rows != {n for n in listed if n.startswith("ADR-")}:
        errs.append("INDEX_DETAIL.md rows differ from the ADR files")
    if not idx.startswith(read(PREAMBLE)):
        errs.append("DECISIONS.md preamble differs from _other/_preamble.md")
    body = read(PREAMBLE) + "".join(read(DIR / n) for n in order["original_files"])
    if sha256(body) != order["original_sha256"]:
        errs.append("original sections no longer reassemble to the recorded sha256")
    have = set(nums)
    gaps = [n for n in range(1, max(nums)) if n not in have]
    if gaps:
        warns.append(f"{len(gaps)} unassigned numbers below {max(nums)} (e.g. {gaps[:5]})")
    for w in warns:
        print("WARN:", w)
    for e in errs:
        print("FAIL:", e)
    if errs:
        sys.exit(1)
    print(f"OK: {len(nums)} ADR files, index consistent")


if __name__ == "__main__":
    main()
