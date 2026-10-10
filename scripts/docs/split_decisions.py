#!/usr/bin/env python3
"""Split docs/DECISIONS.md losslessly into docs/decisions/ADR-NNNN-<slug>.md (one per `## ADR-N:` section),
non-ADR sections and the preamble into docs/decisions/_other/, then replace DECISIONS.md with an index.

Deterministic and idempotent: once DECISIONS.md is the index, re-running only re-verifies that the
split files reassemble (in the recorded order) to the sha256 of the original. Fails loudly otherwise."""
import sys
from adr_lib import *


def split(text):
    starts = [m.start() for m in HEAD_RE.finditer(text)]
    pre = text[: starts[0]]
    secs = [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]
    return pre, secs


def verify():
    order = load_order()
    names = order["original_files"]
    body = read(PREAMBLE) + "".join(read(DIR / n) for n in names)
    got = sha256(body)
    if got != order["original_sha256"]:
        sys.exit(f"FAIL: reassembly sha256 {got} != original {order['original_sha256']}")
    print(f"OK: {len(names)} sections reassemble to the original sha256 {got[:16]}…")


def main():
    cur = read(DECISIONS)
    if INDEX_MARK in cur:
        verify()
        return
    pre, secs = split(cur)
    seen, files, entries, others, rows = {}, [], [], [], []
    for sec in secs:
        head = sec.split("\n", 1)[0]
        m = ADR_HEAD_RE.match(head)
        if m:
            num, title = int(m.group(1)), m.group(2)
            seen[num] = seen.get(num, 0) + 1
            dup = f"-dup{seen[num]}" if seen[num] > 1 else ""
            name = f"ADR-{num:04d}{dup}-{slugify(title)}.md"
            if dup:
                print(f"duplicate number reported: ADR-{num:04d} -> {name}")
            write(DIR / name, sec)
            files.append(name)
            entries.append((name, num, title))
            rows.append((name, num, title) + fields(sec))
        else:
            title = head[3:].strip()
            name = f"_other/{slugify(title)}.md"
            write(DIR / name, sec)
            files.append(name)
            others.append((name.split("/", 1)[1], title))
    write(PREAMBLE, pre)
    body = pre + "".join(secs)
    if body != cur:
        sys.exit("FAIL: split pieces do not concatenate to the original")
    order = {"original_sha256": sha256(cur), "preamble": "_other/_preamble.md",
             "original_files": files, "appended": []}
    write(ORDER, __import__("json").dumps(order, indent=1) + "\n")
    write(DETAIL, render_detail(rows))
    write(DECISIONS, pre + render_index(entries, others))
    verify()
    cov = {k: sum(1 for r in rows if r[i]) for k, i in (("date", 3), ("status", 4), ("decision", 5), ("superseded", 6))}
    print(f"ADRs {len(entries)}, non-ADR {len(others)}; index field coverage {cov}")


if __name__ == "__main__":
    main()
