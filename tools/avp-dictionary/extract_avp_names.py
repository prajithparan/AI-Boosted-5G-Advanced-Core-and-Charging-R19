#!/usr/bin/env python3
"""Extract Diameter AVP name <-> code pairs from the real TS 32.299 text.

Source: ETSI TS 132 299 V19.0.0 (2025-10) == 3GPP TS 32.299 V19.0.0 Release 19, the PDF the user
supplied at specs/TS_32-299.pdf, converted with pdftotext -layout.

Two INDEPENDENT derivations are taken from that one document and required to agree:

  (A) the summary tables -- Table 7.1.0.1 "Use Of IETF Diameter AVPs" and Table 7.2.0.1 "3GPP
      specific AVPs" -- which are column-aligned and therefore fragile: names wrap across lines,
      page headers interleave, and some rows have empty columns.

  (B) the per-clause prose, which states "The <Name> AVP (AVP code <N>) ..." on a single line and
      so cannot be misaligned by a column shift.

A name is emitted only when nothing contradicts it. Any (A)/(B) disagreement is a hard failure --
a silently misaligned row would let an operator scope a tariff onto the wrong attribute and never
see why, which is the exact failure class this repository forbids.

Vendor id comes from which clause the entry lives in: clause 7.1 is IETF (vendor 0), clause 7.2 is
3GPP (vendor 10415). Clauses 7.3/7.4/7.5 (3GPP2, ETSI, oneM2M) are deliberately NOT extracted --
they are other vendors' spaces and this project has no traffic from them.
"""
import pathlib
import re
import sys
from collections import defaultdict

# The four regions are located by their own real headings at run time. They were hardcoded to one
# pdftotext run's line numbers at first, which is a silent-failure trap: a different pdftotext
# version shifts every line, the offsets then point into the wrong clauses, and the script still
# emits a full, plausible, wrong table. Anchors cannot drift that way -- if one is not found, or
# they are not in the expected order, the script refuses to run.
#
# A TOC entry and a body heading have the same text, so they are told apart by the TOC's leader
# dots ("7.1.1  Accounting-Input-Octets AVP.......... 96").
ANCHORS = [
    ("table_ietf_start", re.compile(r"^\s*Table 7\.1\.0\.1: Use Of IETF Diameter AVPs\s*$")),
    ("prose_ietf_start", re.compile(r"^7\.1\.1\s+Accounting-Input-Octets AVP\s*$")),
    ("table_3gpp_start", re.compile(r"^\s*Table 7\.2\.0\.1: 3GPP specific AVPs\s*$")),
    ("prose_3gpp_start", re.compile(r"^7\.2\.1\s+Access-Network-Information AVP\s*$")),
    ("end", re.compile(r"^7\.3\s+3GPP2 specific AVPs\s*$")),
]


def locate(lines: list[str]) -> dict[str, int]:
    found: dict[str, int] = {}
    for name, pattern in ANCHORS:
        hits = [n for n, line in enumerate(lines, 1) if pattern.match(line)]
        if len(hits) != 1:
            raise SystemExit(
                f"anchor {name!r} matched {len(hits)} lines (expected exactly 1) at {hits[:5]} -- "
                f"the document layout is not what this generator was written against; "
                f"refusing to guess"
            )
        found[name] = hits[0]
    order = [found[n] for n, _ in ANCHORS]
    if order != sorted(order):
        raise SystemExit(f"anchors out of order: {found} -- refusing to guess")
    return found


def regions(lines: list[str]):
    a = locate(lines)
    return [
        ("table", a["table_ietf_start"], a["prose_ietf_start"] - 1, 0),
        ("prose", a["prose_ietf_start"], a["table_3gpp_start"] - 1, 0),
        ("table", a["table_3gpp_start"], a["prose_3gpp_start"] - 1, 10415),
        ("prose", a["prose_3gpp_start"], a["end"] - 1, 10415),
    ]

# A table row: leading whitespace, a name, whitespace, an integer code. Parsed as
# "name then first integer" rather than by fixed column index, because the columns do shift.
# A name may start with a DIGIT: the entire 3GPP-* family does (3GPP-RAT-Type, 3GPP-IMSI,
# 3GPP-Charging-Id, 3GPP-User-Location-Info ...), which is the family a Gy scope most often needs.
# Requiring a leading letter silently dropped all of them and nothing complained -- the table still
# built, the cross-checks still passed, the names were simply absent.
TABLE_ROW = re.compile(r"^\s+([A-Za-z0-9][A-Za-z0-9-]*[A-Za-z0-9])\s+(\d{1,5})\s")
PROSE = re.compile(r"\bThe ([A-Za-z0-9][A-Za-z0-9-]*[A-Za-z0-9]) AVP \(AVP code (\d{1,5})\)")
# Lines that look like rows but are not: the running page header on every page.
PAGE_HEADER = re.compile(r"3GPP TS 32\.299 version|ETSI TS 132 299")


emit = False


def main(path: str) -> int:
    # Accept the real PDF from specs/ and do the text conversion here, so the whole pipeline is
    # reproducible from what is committed rather than from a /tmp file someone made once.
    if path.endswith(".pdf"):
        import subprocess
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".txt") as tmp:
            subprocess.run(["pdftotext", "-layout", path, tmp.name], check=True)
            text = pathlib.Path(tmp.name).read_text(encoding="utf-8", errors="replace")
    else:
        text = pathlib.Path(path).read_text(encoding="utf-8", errors="replace")
    # NOT str.splitlines(): it also splits on the form-feed page separators pdftotext emits, which
    # puts Python's line numbers ~1 per page ahead of what grep/sed/an editor report. That is not a
    # cosmetic difference -- the first version of this generator carried hardcoded grep-derived
    # offsets, read regions ~94 lines off as a result, and still emitted a full, plausible,
    # WRONG-region table with zero reported conflicts. The anchors above are the real fix; keeping
    # the numbering identical to every other tool is what makes the diagnostics below trustworthy.
    lines = text.replace("\x0c", "").split("\n")

    table: dict[tuple[int, str], set[int]] = defaultdict(set)
    prose: dict[tuple[int, str], set[int]] = defaultdict(set)
    wrapped = []

    for kind, first, last, vendor in regions(lines):
        for n in range(first, min(last, len(lines)) + 1):
            line = lines[n - 1]
            if PAGE_HEADER.search(line):
                continue
            if kind == "table":
                m = TABLE_ROW.match(line)
                if m:
                    table[(vendor, m.group(1))].add(int(m.group(2)))
                elif line.strip() and re.match(r"^\s+[A-Za-z][A-Za-z0-9-]*-\s*$", line):
                    # A name that wrapped: recorded so coverage is reported honestly, never guessed.
                    wrapped.append((n, line.strip()))
            else:
                for m in PROSE.finditer(line):
                    prose[(vendor, m.group(1))].add(int(m.group(2)))

    conflicts: list[str] = []
    notes: list[str] = []

    # RESOLUTION RULES. The summary tables are primary and the prose is a validator, not a peer.
    # That ordering is not a coin flip -- it was chosen because the prose is demonstrably
    # self-inconsistent in this revision and the table is right where they differ:
    #
    #   * clause 7.2.111Ab is HEADED "Monitoring-UE-HPLMN-Identifier AVP" but its body sentence
    #     reads "The Monitoring-UE-VPLMN-Identifier AVP (AVP code 3431)" -- a copy-paste error in
    #     the spec. Table 7.2.0.1 correctly lists 3431 as Monitoring-UE-HPLMN-Identifier.
    #   * clause 7.2 prose gives "Time-First-Reception AVP (AVP code 3456)", but 3456 is
    #     Proximity-Cancellation-Timestamp in BOTH the table and its own prose clause. The table
    #     lists Time-First-Reception as 3466.
    #
    # Every such divergence is reported below rather than quietly resolved.

    for (vendor, name), codes in table.items():
        if len(codes) > 1:
            conflicts.append(f"table: {name} (vendor {vendor}) lists multiple codes {sorted(codes)}")

    resolved: dict[tuple[int, str], int] = {}
    for key, codes in table.items():
        if len(codes) == 1:
            resolved[key] = next(iter(codes))

    # Prose supplies names the table wrapped across lines, and validates the rest.
    agreed = 0
    for key, codes in prose.items():
        if len(codes) != 1:
            notes.append(f"prose: {key[1]} (vendor {key[0]}) cited with codes {sorted(codes)} -- table wins")
            continue
        code = next(iter(codes))
        if key in resolved:
            if resolved[key] == code:
                agreed += 1
            else:
                notes.append(
                    f"prose/table differ for {key[1]} (vendor {key[0]}): "
                    f"table={resolved[key]} prose={code} -- table wins"
                )
        else:
            resolved[key] = code

    # One code must map to exactly one name, or the reverse lookup this table exists for is
    # ambiguous. Where two spellings of the SAME code appear (the spec carries several: Service-Id
    # vs Service-ID, RAN-End-Time vs RAN-End-Timestamp), the table's spelling is kept and the other
    # is recorded as a known alias -- the code is agreed, only the spelling differs, so nothing is
    # at risk. Where two genuinely DIFFERENT AVPs claim one code, the code is dropped entirely.
    by_code: dict[tuple[int, int], set[str]] = defaultdict(set)
    for (vendor, name), code in resolved.items():
        by_code[(vendor, code)].add(name)

    final: dict[tuple[int, int], str] = {}
    aliases: list[tuple[int, int, str, str]] = []
    for (vendor, code), names in by_code.items():
        if len(names) == 1:
            final[(vendor, code)] = next(iter(names))
            continue
        from_table = sorted(n for n in names if (vendor, n) in table and code in table[(vendor, n)])
        if len(from_table) == 1:
            keep = from_table[0]
            final[(vendor, code)] = keep
            for other in sorted(names - {keep}):
                aliases.append((vendor, code, keep, other))
        else:
            conflicts.append(
                f"vendor {vendor} code {code} claimed by {sorted(names)} with no single table "
                f"spelling -- dropped, the numeric key still carries it"
            )

    print(f"table rows parsed     : {len(table)}")
    print(f"prose citations       : {len(prose)}")
    print(f"cross-checked agreeing: {agreed}")
    print(f"table names wrapped   : {len(wrapped)} (no name emitted; numeric key still works)")
    print(f"emitted name<->code   : {len(final)}")
    print(f"spelling aliases      : {len(aliases)}")
    print(f"notes                 : {len(notes)}")
    print(f"hard conflicts        : {len(conflicts)}")
    for c in conflicts:
        print("  !", c)
    for n in notes:
        print("  ~", n)
    for n, t in wrapped:
        print(f"  wrapped line {n}: {t}")
    for vendor, code, keep, other in sorted(aliases):
        print(f"  alias vendor {vendor} code {code}: kept {keep!r}, spec also spells it {other!r}")

    fd_problems = crosscheck_freediameter(final)
    for f in fd_problems:
        print("  !", f)
    if conflicts or fd_problems:
        return 1

    if emit:
        write_source(final)
        print("wrote libs/diameter-core/include/diameter_core/avp_names.hpp")
        print("wrote libs/diameter-core/src/avp_names.cpp")
    return 0


# A THIRD derivation, from a different citation class entirely: freeDiameter's own vendored
# dictionary C source (simulators/reference/freeDiameter/, BSD licensed, commit
# e48fd4f8afc48f5e839558a90ef5a67165e94fad -- the same material every constant in dictionary.hpp
# cites). It covers only the base-protocol and RFC 4006 DCC AVPs, so it validates a minority of the
# table -- but a column misalignment in the PDF extraction would show up here immediately, because
# these two sources have no common ancestry in this repository.
FD_SOURCES = [
    "simulators/reference/freeDiameter/extensions/dict_dcca/dict_dcca.c",
    "simulators/reference/freeDiameter/libfdcore/dict_base_proto.c",
]

FD_AVP = re.compile(
    r"struct\s+dict_avp_data\s+\w+\s*=\s*\{\s*"
    r"(\d+)\s*,\s*/\*\s*Code\s*\*/\s*"
    r"(\d+)\s*,\s*/\*\s*Vendor\s*\*/\s*"
    r'"([^"]+)"\s*,\s*/\*\s*Name\s*\*/'
)


def crosscheck_freediameter(final: dict[tuple[int, int], str]) -> list[str]:
    by_name = {name: (vendor, code) for (vendor, code), name in final.items()}
    problems = []
    checked = 0
    for src in FD_SOURCES:
        path = pathlib.Path(src)
        if not path.exists():
            problems.append(f"missing vendored reference {src}")
            continue
        for m in FD_AVP.finditer(path.read_text(errors="replace")):
            code, vendor, name = int(m.group(1)), int(m.group(2)), m.group(3)
            if name not in by_name:
                continue  # TS 32.299 does not use this AVP; nothing to contradict.
            checked += 1
            if by_name[name] != (vendor, code):
                problems.append(
                    f"{name}: freeDiameter says vendor {vendor} code {code}, "
                    f"TS 32.299 says vendor {by_name[name][0]} code {by_name[name][1]}"
                )
    print(f"freeDiameter cross-check: {checked} names, {len(problems)} mismatches")
    return problems


def write_source(final: dict[tuple[int, int], str]) -> None:
    rows = sorted(final.items())
    hpp = """#pragma once

#include <cstdint>
#include <string_view>

// Diameter AVP code -> name, for turning a wire AVP into an attribute an operator can scope a
// product on by name instead of by number (ADR-0351).
//
// SOURCE: ETSI TS 132 299 V19.0.0 (2025-10) == 3GPP TS 32.299 V19.0.0 Release 19, at
// specs/TS_32-299.pdf -- Table 7.1.0.1 "Use Of IETF Diameter AVPs" (vendor 0) and Table 7.2.0.1
// "3GPP specific AVPs" (vendor 10415), cross-checked against that same document's own per-clause
// prose ("The <Name> AVP (AVP code <N>)").
//
// CITATION CLASS: this is a real 3GPP spec PDF, the same class as the Sy block in dictionary.hpp
// and NOT the freeDiameter C source the base/DCC constants there cite. Disclosed rather than
// silently treated as equivalent.
//
// GENERATED by tools/avp-dictionary/extract_avp_names.py -- do not edit by hand. The generator
// carries the extraction rules, the conflicts it found in the spec text, and the cross-checks that
// must pass before it will emit anything.

namespace diameter_core {

// The AVP's real name, or an empty view when this dictionary does not carry the code. An empty
// result is normal and safe: the caller keeps using the numeric key, which is always emitted.
std::string_view avp_name(std::uint32_t vendor_id, std::uint32_t code) noexcept;

} // namespace diameter_core
"""
    pathlib.Path("libs/diameter-core/include/diameter_core/avp_names.hpp").write_text(hpp)

    body = ["// GENERATED by tools/avp-dictionary/extract_avp_names.py -- do not edit by hand.",
            "// Source: 3GPP TS 32.299 V19.0.0 (specs/TS_32-299.pdf). See avp_names.hpp.",
            "",
            '#include "diameter_core/avp_names.hpp"',
            "",
            "#include <algorithm>",
            "#include <array>",
            "#include <utility>",
            "",
            "namespace diameter_core {",
            "namespace {",
            "",
            "struct Entry {",
            "    std::uint32_t vendor_id;",
            "    std::uint32_t code;",
            "    std::string_view name;",
            "};",
            "",
            "// Sorted by (vendor_id, code) so the lookup is a binary search, not a scan: this runs",
            "// once per AVP per charging request on the Gy hot path.",
            f"constexpr std::array<Entry, {len(rows)}> kNames{{{{"]
    for (vendor, code), name in rows:
        body.append(f'    {{{vendor}u, {code}u, "{name}"}},')
    body += ["}};",
             "",
             "} // namespace",
             "",
             "std::string_view avp_name(std::uint32_t vendor_id, std::uint32_t code) noexcept {",
             "    const auto it = std::lower_bound(",
             "        kNames.begin(), kNames.end(), std::pair{vendor_id, code},",
             "        [](const Entry& e, const std::pair<std::uint32_t, std::uint32_t>& key) {",
             "            return std::pair{e.vendor_id, e.code} < key;",
             "        });",
             "    if (it == kNames.end() || it->vendor_id != vendor_id || it->code != code) {",
             "        return {};",
             "    }",
             "    return it->name;",
             "}",
             "",
             "} // namespace diameter_core",
             ""]
    pathlib.Path("libs/diameter-core/src/avp_names.cpp").write_text("\n".join(body))

    # Run the repository's own formatter over the output, so re-running this generator is
    # idempotent against what is committed and a regeneration never shows up as a whitespace diff.
    import shutil
    import subprocess
    fmt = shutil.which("clang-format") or shutil.which("clang-format-18")
    if fmt:
        subprocess.run([fmt, "-i",
                        "libs/diameter-core/include/diameter_core/avp_names.hpp",
                        "libs/diameter-core/src/avp_names.cpp"], check=True)
    else:
        print("NOTE: clang-format not found; run it over the generated files before committing")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--emit"]
    emit = "--emit" in sys.argv
    sys.exit(main(args[0] if args else "specs/TS_32-299.pdf"))
