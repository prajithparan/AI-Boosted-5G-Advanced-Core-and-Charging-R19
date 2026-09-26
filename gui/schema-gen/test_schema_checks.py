#!/usr/bin/env python3
"""Negative tests for the schema checks (ADR-0421): a check that never fails proves nothing.

Each case copies the relevant sources into a temp tree, applies one realistic drift, and asserts
the check rejects it. Usage: test_schema_checks.py <repo_root>
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROV_SCHEMA = "gui/web/src/schemas/provisioning-customer-order.schema.json"
TMF_SCHEMA = "gui/web/src/schemas/tmf620.derived.json"
COPY = [
    "bss/provisioning/src/provisioning_store.cpp",
    "deploy/db/charging/20-subscriber.sql",
    "libs/bss-sid/include/bss_sid/product.hpp",
    "gui/schema-gen/overlay-tmf620.json",
    PROV_SCHEMA,
    TMF_SCHEMA,
]


def tree(repo: Path) -> Path:
    tmp = Path(tempfile.mkdtemp(prefix="gui-schema-neg-"))
    for rel in COPY:
        (tmp / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(repo / rel, tmp / rel)
    return tmp


def run(script: str, *args: str) -> int:
    return subprocess.run([sys.executable, str(HERE / script), *args],
                          capture_output=True, text=True).returncode


def mutate_json(path: Path, fn) -> None:
    doc = json.loads(path.read_text())
    fn(doc)
    path.write_text(json.dumps(doc))


def main(argv: list[str]) -> int:
    repo = Path(argv[1]).resolve()
    failures = 0

    def expect(name: str, rc: int, want_fail: bool = True) -> None:
        nonlocal failures
        ok = (rc != 0) if want_fail else (rc == 0)
        print(f"{'ok  ' if ok else 'FAIL'} {name} (rc={rc})")
        failures += 0 if ok else 1

    # Baseline must pass, or every "rejected" below is meaningless.
    t = tree(repo)
    expect("baseline provisioning schema passes", run("check_provisioning_schema.py", str(t)), False)
    expect("baseline tmf620 schema up to date",
           run("derive_tmf620_schema.py", str(t), str(t / TMF_SCHEMA), "--check"), False)
    shutil.rmtree(t)

    prov_cases = {
        "dropped key the service reads (initialBalance.usageType)":
            lambda s: s["properties"]["initialBalance"]["properties"].pop("usageType"),
        "invented field (iccid)":
            lambda s: s["properties"].__setitem__("iccid", {"type": "string"}),
        "pattern drift (msisdn)":
            lambda s: s["properties"]["msisdn"].__setitem__("pattern", "^[0-9]+$"),
        "over-required (msisdn)":
            lambda s: s["required"].append("msisdn"),
        "under-required (sim.opc)":
            lambda s: s["properties"]["sim"]["required"].remove("opc"),
        "enum drift (segment)":
            lambda s: s["properties"]["segment"].__setitem__("enum", ["CONSUMER"]),
        "secret not writeOnly (sim.k)":
            lambda s: s["properties"]["sim"]["properties"]["k"].pop("writeOnly"),
    }
    for name, fn in prov_cases.items():
        t = tree(repo)
        mutate_json(t / PROV_SCHEMA, fn)
        expect(f"provisioning rejects: {name}", run("check_provisioning_schema.py", str(t)))
        shutil.rmtree(t)

    # Source drift: the service starts reading a new key -> the unchanged schema must fail.
    t = tree(repo)
    store = t / "bss/provisioning/src/provisioning_store.cpp"
    store.write_text(store.read_text().replace(
        'jval(request, "msisdn")', 'jval(request, "msisdn") + jval(request, "iccid")', 1))
    expect("provisioning rejects: service reads a key the schema lacks",
           run("check_provisioning_schema.py", str(t)))
    shutil.rmtree(t)

    # TMF620: a DTO field added to the header -> committed schema is stale.
    t = tree(repo)
    hdr = t / "libs/bss-sid/include/bss_sid/product.hpp"
    hdr.write_text(hdr.read_text().replace(
        "    std::optional<std::string> statusReason;",
        "    std::optional<std::string> statusReason;\n    std::optional<std::string> newField;", 1))
    expect("tmf620 drift check rejects: DTO gained a field",
           run("derive_tmf620_schema.py", str(t), str(t / TMF_SCHEMA), "--check"))
    shutil.rmtree(t)

    # TMF620: an overlay rule naming a field the DTO does not have -> generation refuses.
    t = tree(repo)
    mutate_json(t / "gui/schema-gen/overlay-tmf620.json",
                lambda o: o["ProductOffering"].append(
                    {"path": "priceAlteration", "required": True, "source": "made up"}))
    expect("tmf620 overlay rejects: rule on a non-existent field",
           run("derive_tmf620_schema.py", str(t), str(t / "out.json")))
    shutil.rmtree(t)

    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
