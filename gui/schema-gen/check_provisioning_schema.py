#!/usr/bin/env python3
"""Cross-check the GUI's customer-order schema against bss/provisioning's real source (ADR-0421).

The provisioning request is the project's own OAM API: no generated DTO exists, so the schema is
hand-transcribed. This check re-derives, independently, from the C++ and SQL the service actually
runs:
  * every key the service reads (jval(obj,"k") / obj.value("k", ...)), per nested object;
  * every validation regex, and which field it is matched against;
  * which fields are required (validated unconditionally, not behind `!x.empty() &&`);
  * the segment / chargingMode enums from the charging DB CHECK constraints.
and fails if the schema disagrees in any direction (a key the service ignores is as wrong as a key
the schema is missing).

Usage: check_provisioning_schema.py <repo_root>
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

STORE = "bss/provisioning/src/provisioning_store.cpp"
SCHEMA = "gui/web/src/schemas/provisioning-customer-order.schema.json"
SUBSCRIBER_SQL = "deploy/db/charging/20-subscriber.sql"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def keys_read(src: str) -> dict[str, set[str]]:
    """Map schema object path ('' = root) -> set of keys the service reads from it."""
    # `const json indiv = request.value("individual", ...)` binds a C++ variable to a sub-object.
    var_to_path = {"request": ""}
    for var, key in re.findall(r"const\s+json\s+(\w+)\s*=\s*request\.value\(\"(\w+)\"", src):
        var_to_path[var] = key
    out: dict[str, set[str]] = {p: set() for p in var_to_path.values()}
    for var, key in re.findall(r"jval\((\w+),\s*\"(\w+)\"", src):
        if var in var_to_path:
            out[var_to_path[var]].add(key)
    for var, key in re.findall(r"\b(\w+)\.value\(\"(\w+)\"", src):
        if var in var_to_path:
            out[var_to_path[var]].add(key)
    return out


def regex_checks(src: str) -> tuple[dict[str, str], set[str]]:
    """field path -> regex literal, and the set of fields validated unconditionally."""
    literals = dict(re.findall(r"std::regex\s+(\w+)\(\"([^\"]+)\"\)", src))
    patterns: dict[str, str] = {}
    required: set[str] = set()
    for guard, expr, rx in re.findall(
        r"(!\s*[\w.]+\.empty\(\)\s*&&\s*)?!std::regex_match\(([\w.]+),\s*(\w+)\)", src
    ):
        field = expr.replace("spec.sim.", "sim.")
        if rx not in literals:
            fail(f"regex variable {rx} has no literal in {STORE}")
        patterns[field] = literals[rx]
        if not guard:
            required.add(field)
    return patterns, required


def check_enum(schema_prop: dict, sql: str, column: str) -> None:
    m = re.search(rf"CHECK\s*\(\s*{column}\s+IN\s*\(([^)]*)\)", sql)
    if m is None:
        fail(f"no CHECK constraint on {column} in {SUBSCRIBER_SQL}")
    allowed = re.findall(r"'([^']+)'", m.group(1))
    if schema_prop.get("enum") != allowed:
        fail(f"enum for {column}: schema {schema_prop.get('enum')} != DB CHECK {allowed}")


def main(argv: list[str]) -> int:
    repo = Path(argv[1]) if len(argv) > 1 else Path(".")
    src = (repo / STORE).read_text()
    schema = json.loads((repo / SCHEMA).read_text())
    sql = (repo / SUBSCRIBER_SQL).read_text()

    # 1. keys: the schema's property set per object must equal what the service reads.
    for path, keys in keys_read(src).items():
        node = schema if path == "" else schema["properties"].get(path)
        if node is None:
            fail(f"service reads object '{path}' but the schema has no such property")
        props = set(node.get("properties", {}))
        expected = set(keys)
        if path == "":
            # sub-objects are read via request.value("x", ...) and appear as keys of the root
            expected |= {p for p in keys_read(src) if p}
        if props != expected:
            fail(f"object '{path or '<root>'}': schema {sorted(props)} != service reads {sorted(expected)}")

    # 2. patterns + 3. required.
    patterns, required = regex_checks(src)
    if not patterns:
        fail("found no regex validation in the store -- the parser no longer matches the source")
    for field, rx in patterns.items():
        parts = field.split(".")
        node = schema
        for p in parts:
            node = node["properties"][p]
        if node.get("pattern") != rx:
            fail(f"{field}: schema pattern {node.get('pattern')!r} != service regex {rx!r}")
    for field in required:
        parts = field.split(".")
        parent = schema
        for p in parts[:-1]:
            if p not in parent.get("required", []):
                fail(f"{field} is validated unconditionally, so '{p}' must be required")
            parent = parent["properties"][p]
        if parts[-1] not in parent.get("required", []):
            fail(f"{field} is validated unconditionally but not required in the schema")
    # nothing may be required that the service does not require
    declared = {r for r in schema.get("required", [])}
    declared |= {f"sim.{r}" for r in schema["properties"]["sim"].get("required", [])}
    derived = required | {f.split(".")[0] for f in required}
    if declared != derived:
        fail(f"required: schema {sorted(declared)} != service {sorted(derived)}")

    # 4. enums from the DB CHECK constraints.
    check_enum(schema["properties"]["segment"], sql, "account_kind")
    check_enum(schema["properties"]["chargingMode"], sql, "charging_mode")

    # 5. secrets are marked writeOnly (drives the GUI's masked, never-echoed input).
    for secret in ("k", "opc"):
        if schema["properties"]["sim"]["properties"][secret].get("writeOnly") is not True:
            fail(f"sim.{secret} must be writeOnly")

    print(f"{SCHEMA}: consistent with {STORE} and {SUBSCRIBER_SQL} "
          f"({sum(len(v) for v in keys_read(src).values())} keys, {len(patterns)} patterns)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
