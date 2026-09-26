#!/usr/bin/env python3
"""Derive the operator GUI's TMF620 JSON Schemas from the real DTO definitions (ADR-0421).

Source of truth: libs/bss-sid/include/bss_sid/product.hpp -- the C++ structs bss/product-catalog
(de)serializes every TMF620 request with. Those structs were themselves transcribed from TM Forum's
TMF620 v4.1.0 swagger (see that header's own provenance comment). The GUI must accept exactly what
the service accepts, so the schema is DERIVED from the structs, not re-typed by hand:

  std::string            -> string, REQUIRED   (from_json uses j.at(...), a missing key throws)
  std::optional<T>       -> T, optional        (get_optional)
  std::vector<T>         -> array of T, optional (get_array; empty arrays are omitted on output)
  int / double / bool    -> integer / number / boolean
  nlohmann::json alias   -> {} (any JSON value; the DTO keeps it untyped)
  another struct         -> that struct's schema, inlined (no recursion exists in the header)

Then a small overlay (overlay-tmf620.json) applies the SERVICE-side create rules that the struct
cannot express (e.g. ProductOffering.name required on create, bss/product-catalog/src/store.cpp).
Every overlay entry must name a path that already exists in the derived schema -- the overlay can
constrain, annotate or hide a field, never add one. That is enforced here (hard error) and in the
drift test.

The C++ round-trip test (gui/bff/tests/test_tmf620_schema_roundtrip.cpp) is the independent
check: it builds a maximal instance from the emitted schema and pushes it through the real
bss_sid from_json/to_json, which fails if any key or type here disagrees with the DTO.

Usage:
  derive_tmf620_schema.py <repo_root> <out.json>        write the schema file
  derive_tmf620_schema.py <repo_root> <out.json> --check exit 1 if <out.json> is stale (ctest)
"""

from __future__ import annotations

import copy
import json
import re
import sys
from pathlib import Path

HEADER = "libs/bss-sid/include/bss_sid/product.hpp"
OVERLAY = "gui/schema-gen/overlay-tmf620.json"
# The two resources the first GUI increment creates/lists (ADR-0420). ProductSpecification is
# derivable the same way; it is simply not a screen yet.
ROOTS = ["ProductOffering", "ProductOfferingPrice"]

PRIMITIVES = {
    "std::string": {"type": "string"},
    "int": {"type": "integer"},
    "double": {"type": "number"},
    "bool": {"type": "boolean"},
    "nlohmann::json": {},
}


def strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*", "", text)


def parse_header(path: Path) -> tuple[dict[str, list[tuple[str, str]]], set[str]]:
    text = strip_comments(path.read_text())
    aliases = set(re.findall(r"using\s+(\w+)\s*=\s*nlohmann::json\s*;", text))
    structs: dict[str, list[tuple[str, str]]] = {}
    for m in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\};", text, re.S):
        name, body = m.group(1), m.group(2)
        fields = []
        for decl in body.split(";"):
            decl = " ".join(decl.split())
            if not decl:
                continue
            fm = re.fullmatch(r"(.+?)\s+(\w+)", decl)
            if fm is None:
                raise SystemExit(f"{path}: cannot parse member '{decl}' of struct {name}")
            fields.append((fm.group(1).replace(" ", ""), fm.group(2)))
        structs[name] = fields
    return structs, aliases


def type_schema(cpp_type: str, structs, aliases) -> dict:
    if cpp_type in PRIMITIVES:
        return copy.deepcopy(PRIMITIVES[cpp_type])
    if cpp_type in aliases:
        return {"$comment": f"untyped JSON in the DTO ({cpp_type} = nlohmann::json)"}
    if cpp_type in structs:
        return struct_schema(cpp_type, structs, aliases)
    raise SystemExit(f"unmapped C++ type '{cpp_type}' -- extend the mapping, do not guess")


def struct_schema(name: str, structs, aliases) -> dict:
    props: dict[str, dict] = {}
    required: list[str] = []
    for cpp_type, field in structs[name]:
        if m := re.fullmatch(r"std::optional<(.+)>", cpp_type):
            props[field] = type_schema(m.group(1), structs, aliases)
        elif m := re.fullmatch(r"std::vector<(.+)>", cpp_type):
            props[field] = {"type": "array", "items": type_schema(m.group(1), structs, aliases)}
        else:
            props[field] = type_schema(cpp_type, structs, aliases)
            if cpp_type == "std::string":
                required.append(field)
            else:
                raise SystemExit(f"{name}.{field}: non-optional non-string member, unmapped")
    out: dict = {"type": "object", "title": name, "properties": props}
    if required:
        out["required"] = required
    return out


def resolve(schema: dict, dotted: str) -> dict:
    """Walk 'a.b[].c' to the property schema; hard error if any segment does not exist."""
    node = schema
    for seg in dotted.split("."):
        is_array = seg.endswith("[]")
        key = seg[:-2] if is_array else seg
        props = node.get("properties", {})
        if key not in props:
            raise SystemExit(f"overlay path '{dotted}': '{key}' is not a field of the derived DTO")
        node = props[key]
        if is_array:
            if node.get("type") != "array":
                raise SystemExit(f"overlay path '{dotted}': '{key}' is not an array")
            node = node["items"]
    return node


def apply_overlay(root: str, schema: dict, overlay: dict) -> None:
    for rule in overlay.get(root, []):
        path, cite = rule["path"], rule["source"]
        if not cite:
            raise SystemExit(f"overlay {root}.{path}: every rule must cite its source")
        if "." in path or "[]" in path:
            parent_path, leaf = path.rsplit(".", 1)
            parent = resolve(schema, parent_path)
        else:
            parent, leaf = schema, path
        resolve(parent, leaf)  # existence check
        target = parent["properties"][leaf]
        if rule.get("required"):
            req = parent.setdefault("required", [])
            if leaf not in req:
                req.append(leaf)
            # An empty string is not a name: store.cpp rejects "" as well as absent.
            if target.get("type") == "string":
                target["minLength"] = 1
        for key in ("readOnly", "format", "description"):
            if key in rule:
                target[key] = rule[key]
        if rule.get("x-ui-hidden"):
            target["x-ui-hidden"] = True


def derive(repo: Path) -> dict:
    structs, aliases = parse_header(repo / HEADER)
    overlay = json.loads((repo / OVERLAY).read_text())
    out = {
        "$comment": (
            "GENERATED by gui/schema-gen/derive_tmf620_schema.py from "
            f"{HEADER} + {OVERLAY} (ADR-0421). Do not edit; regenerate."
        ),
        "definitions": {},
    }
    for root in ROOTS:
        s = struct_schema(root, structs, aliases)
        s["$schema"] = "http://json-schema.org/draft-07/schema#"
        apply_overlay(root, s, overlay)
        out["definitions"][root] = s
    return out


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 2
    repo, out_path = Path(argv[1]), Path(argv[2])
    rendered = json.dumps(derive(repo), indent=2, sort_keys=False) + "\n"
    if "--check" in argv[3:]:
        current = out_path.read_text() if out_path.exists() else ""
        if current != rendered:
            print(f"{out_path} is stale relative to {HEADER}/{OVERLAY}; "
                  "run gui/schema-gen/derive_tmf620_schema.py to regenerate", file=sys.stderr)
            return 1
        print(f"{out_path}: up to date")
        return 0
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(rendered)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
