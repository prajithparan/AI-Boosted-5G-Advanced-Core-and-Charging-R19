#!/usr/bin/env python3
"""Bind a date into nfs/chf/feature_extract.sql and emit runnable SQL.

ADR-0350 shipped the extraction query with a `:feature_date` placeholder and nothing that binds
it -- the query existed, no way to run it did, and the feature store stayed empty while the CDR
table filled up. This is that missing half.

SQL is written to stdout rather than executed, deliberately: the Doris endpoint differs between a
local lab (a `mysql` client inside the container) and CI (the mariadb-client on the runner), and a
tool that hardcodes one cannot run in the other. The caller pipes:

    python3 tools/chf-features/extract_features.py --date 2026-09-12 \
        | mysql -h127.0.0.1 -P9030 -uroot

Re-running a date is safe: chf_features.subscriber_features is a Doris UNIQUE KEY table keyed on
(feature_date, subscriber_identifier), so a second run replaces that day's rows rather than
doubling them. That matters because a day is only complete once its last CDR has landed, and the
honest way to handle a day extracted too early is to extract it again.
"""
import argparse
import datetime
import pathlib
import re
import sys

QUERY = pathlib.Path(__file__).resolve().parents[2] / "nfs" / "chf" / "feature_extract.sql"

# ISO date only. The value is substituted into SQL, so it is validated rather than trusted --
# anything that is not exactly a date is rejected instead of being quoted and hoped about.
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def sql_for(date: str) -> str:
    if not DATE.match(date):
        raise SystemExit(f"not an ISO date: {date!r}")
    try:
        datetime.date.fromisoformat(date)
    except ValueError as exc:
        raise SystemExit(f"not a real date: {date!r} ({exc})") from exc
    text = QUERY.read_text(encoding="utf-8")
    if ":feature_date" not in text:
        raise SystemExit(f"{QUERY} no longer contains the :feature_date placeholder")
    return text.replace(":feature_date", f"'{date}'")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", action="append", required=True, metavar="YYYY-MM-DD",
                        help="feature date to extract; repeatable")
    args = parser.parse_args()
    for date in args.date:
        print(f"-- feature_date {date}")
        print(sql_for(date))
    return 0


if __name__ == "__main__":
    sys.exit(main())
