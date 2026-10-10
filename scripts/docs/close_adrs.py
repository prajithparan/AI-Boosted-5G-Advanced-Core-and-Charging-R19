#!/usr/bin/env python3
"""Mark ADRs Closed and move them to docs/decisions/archive/ (ADR-0465).

usage: close_adrs.py LIST [--date YYYY-MM-DD] [--dry-run]
LIST has one `<adr-number> <commit>` per line: the latest commit on origin/main that cites the ADR.

The existing status text is never overwritten: `Closed (...)` is put in front of it (or added where the
ADR had no status). Then the file is `git mv`-ed into archive/ and its DECISIONS.md line, INDEX_DETAIL.md
row and _order.json entry are rewritten. Run check_adr_index.py afterwards."""
import argparse
import datetime
import json
import re
import subprocess
import sys
from adr_lib import *


def mark_closed(text, commit, date):
    tag = f"Closed (work pushed; last citing commit {commit} on origin/main, {date})."
    if re.search(r"\*\*Status:\*\*", text):
        return re.sub(r"(\*\*Status:\*\*\s*)", lambda m: m.group(1) + tag + " ", text, count=1)
    m = re.search(r"^.*\*\*Date:\*\*.*$", text, re.M)
    if m:
        line = m.group(0).rstrip()
        sep = " " if line.endswith(".") else ". "
        return text.replace(m.group(0), line + sep + f"**Status:** {tag}", 1)
    head, _, rest = text.partition("\n")
    return f"{head}\n\n**Status:** {tag}\n{rest}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("list")
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    wanted = dict(l.split() for l in open(a.list) if l.strip())
    wanted = {int(k): v for k, v in wanted.items()}
    by_num = {adr_number(p.name): p for p in adr_files() if p.parent == DIR}
    missing = sorted(set(wanted) - set(by_num))
    if missing:
        sys.exit(f"not live in docs/decisions/ (already archived or absent): {missing}")
    order = load_order()
    idx, detail = read(DECISIONS), read(DETAIL)
    for num, commit in sorted(wanted.items()):
        src = by_num[num]
        name = src.name
        text = mark_closed(read(src), commit, a.date)
        date, status, decision, sup = fields(text)
        assert status.startswith("Closed ("), (num, status)
        if a.dry_run:
            print(num, status)
            continue
        write(src, text)
        ARCHIVE.mkdir(exist_ok=True)
        subprocess.run(["git", "mv", str(src), str(ARCHIVE / name)], check=True)
        link = f"](decisions/{name})"
        assert link in idx, name
        idx = idx.replace(link, f"](decisions/archive/{name})")
        row = re.compile(rf"^\| {num:04d} \|.*$", re.M)
        assert row.search(detail), num
        detail = row.sub(lambda m: f"| {num:04d} | {cell(status)} | {date} | {cell(title_of(ARCHIVE / name))} | "
                         f"{cell(decision)} | {sup} | [{name}](archive/{name}) |", detail, count=1)
        for key in ("original_files", "appended"):
            order[key] = [f"archive/{name}" if n == name else n for n in order[key]]
    if not a.dry_run:
        write(DECISIONS, idx)
        write(DETAIL, detail)
        write(ORDER, json.dumps(order, indent=1) + "\n")
        print(f"closed and archived {len(wanted)} ADRs")


if __name__ == "__main__":
    main()
