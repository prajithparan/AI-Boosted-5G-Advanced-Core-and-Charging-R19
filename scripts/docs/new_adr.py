#!/usr/bin/env python3
"""Create the next-numbered ADR file from a template and register it in the index.

usage: new_adr.py "title" [--date YYYY-MM-DD]
Writes docs/decisions/ADR-NNNN-<slug>.md, appends a line to docs/DECISIONS.md, a row to
docs/decisions/INDEX_DETAIL.md and the filename to docs/decisions/_order.json ("appended")."""
import argparse
import datetime
import json
import re
import sys
from adr_lib import *

TEMPLATE = """## ADR-{num:04d}: {title}

**Date:** {date}. **Status:** Proposed.

**Context.** 

**Decision:** 

**Rejected alternatives.** 
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("title")
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    a = ap.parse_args()
    nums = [adr_number(p.name) for p in adr_files()]
    num = max(nums) + 1
    name = f"ADR-{num:04d}-{slugify(a.title)}.md"
    sec = TEMPLATE.format(num=num, title=a.title, date=a.date)
    write(DIR / name, sec)
    idx = read(DECISIONS)
    line = f"- [{num:04d}](decisions/{name}) {a.title}\n"
    marker = "\nNon-ADR sections"
    if marker in idx:
        head, tail = idx.split(marker, 1)
        idx = head.rstrip("\n") + "\n" + line + "\n" + marker.lstrip("\n") + tail
    else:
        idx = idx.rstrip("\n") + "\n" + line
    write(DECISIONS, idx)
    d = read(DETAIL)
    date, status, decision, sup = fields(sec)
    write(DETAIL, d.rstrip("\n") + "\n" + f"| {num:04d} | {cell(status)} | {date} | {cell(a.title)} | {cell(decision)} | {sup} | [{name}]({name}) |\n")
    order = load_order()
    order["appended"].append(name)
    write(ORDER, json.dumps(order, indent=1) + "\n")
    print(f"created docs/decisions/{name}")


if __name__ == "__main__":
    main()
