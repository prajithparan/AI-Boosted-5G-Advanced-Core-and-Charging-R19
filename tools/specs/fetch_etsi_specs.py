#!/usr/bin/env python3
"""Fetch the ETSI LI specifications this project reads, and convert them to text.

Why a fetcher and not a commit: ETSI deliverables carry "No part may be reproduced or utilized in
any form or by any means ... except as authorized by written permission of ETSI", so unlike the
3GPP material under specs/3gpp they are not committed (specs/etsi/*.pdf and *.txt are gitignored;
see specs/etsi/SOURCES.md). This script recreates the exact versions the code cites.

Source: https://www.etsi.org/deliver/ -- ETSI's own server. It refuses the default Python/curl
user agents (403), hence the browser-like UA. The path layout is ETSI's:
    etsi_ts/<range>/<spec-digits>/<MM.mm.pp>_60/ts_<spec-digits>v<MMmmpp>p.pdf
where "_60" is ETSI's status code for a published deliverable.

Text conversion is pdftotext -layout (poppler-utils): the tables that carry the normative field
lists (e.g. TS 103 221-2 table 5.1-1) survive it column-aligned, which the code comments cite.
"""
import pathlib
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "specs/etsi"
UA = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) spec-fetch 5gc-r19"}

# (spec digits, version, local file stem). Versions are the ones cited in docs/DECISIONS.md
# ADR-0364; bump here and there together.
SPECS = [
    ("10322101", "01.23.01", "TS_103_221-1_v1.23.1"),
    ("10322102", "01.10.01", "TS_103_221-2_v1.10.1"),
]


def url_for(digits: str, version: str) -> str:
    lo = int(digits[:6]) // 100 * 100
    compact = version.replace(".", "")
    return (f"https://www.etsi.org/deliver/etsi_ts/{lo}_{lo + 99}/{digits}/{version}_60/"
            f"ts_{digits}v{compact}p.pdf")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    have_pdftotext = shutil.which("pdftotext") is not None
    for digits, version, stem in SPECS:
        pdf = OUT / f"{stem}.pdf"
        if not pdf.exists():
            url = url_for(digits, version)
            print(f"fetching {url}")
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=120) as r:
                pdf.write_bytes(r.read())
        txt = OUT / f"{stem}.txt"
        if have_pdftotext and not txt.exists():
            subprocess.run(["pdftotext", "-layout", str(pdf), str(txt)], check=True)
        print(f"{pdf.name}: {pdf.stat().st_size} bytes" + ("" if txt.exists() else " (no pdftotext; text not produced)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
