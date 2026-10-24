#!/usr/bin/env python3
"""Structural schema-conformance check for the JSON samples written by
tests/conformance/test_round_trip.cpp, checked against the *actual* R19
OpenAPI schema (specs/5G_APIs-REL-19/), not a hand-copied expectation.

Scope, disclosed rather than assumed: this checks field-name/required-ness
structural conformance (every instance key is a declared property, every
required property is present) using the real $ref-resolving loader from
tools/sbi-codegen. It does NOT do full JSON Schema type/pattern/format
validation -- pattern enforcement is instead exercised directly in the C++
test (AmfIdPatternValidation) against the generated validate_AmfId function.
A fuller jsonschema-library-based validation (translating OpenAPI's
`nullable: true` to a real JSON Schema dialect) is future work, not silently
assumed done here.

Run as a CTest test via tests/conformance/CMakeLists.txt, after the C++
round-trip test has written its sample files.
"""

from __future__ import annotations

import json
import pathlib
import sys

_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "tools" / "sbi-codegen"))

from sbi_codegen.loader import SchemaRegistry  # noqa: E402


def check_object(schema: dict, instance: dict, registry: SchemaRegistry, from_file: str, path: str) -> list[str]:
    errors = []
    required = set(schema.get("required", []))
    properties = schema.get("properties", {})

    for req in required:
        if req not in instance:
            errors.append(f"{path}: missing required property '{req}'")

    for key, value in instance.items():
        if key not in properties:
            errors.append(f"{path}: property '{key}' is not declared in the schema")
            continue
        errors.extend(check_value(properties[key], value, registry, from_file, f"{path}.{key}"))

    return errors


def check_value(prop_schema: dict, value, registry: SchemaRegistry, from_file: str, path: str) -> list[str]:
    """A property's value: a $ref object, and (ADR-0519) the members of a map
    (additionalProperties: $ref) and the items of an array (items: $ref). A null
    (merge-patch removal) is not checked."""
    if "$ref" in prop_schema and isinstance(value, dict):
        name, resolved, source_file = registry.resolve_ref(prop_schema["$ref"], from_file)
        return check_object(resolved, value, registry, source_file, path)
    errors = []
    extra = prop_schema.get("additionalProperties")
    if isinstance(extra, dict) and "$ref" in extra and isinstance(value, dict):
        for k, v in value.items():
            errors.extend(check_value(extra, v, registry, from_file, f"{path}[{k}]"))
    items = prop_schema.get("items")
    if isinstance(items, dict) and "$ref" in items and isinstance(value, list):
        for i, v in enumerate(value):
            errors.extend(check_value(items, v, registry, from_file, f"{path}[{i}]"))
    return errors


def main() -> int:
    specs_dir = _REPO_ROOT / "specs" / "5G_APIs-REL-19"
    samples_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(".")

    registry = SchemaRegistry(specs_dir)
    registry.load_file("TS29571_CommonData.yaml")
    registry.load_file("TS29510_Nnrf_NFManagement.yaml")

    all_errors: list[str] = []

    guami_path = samples_dir / "guami.json"
    if not guami_path.exists():
        print(f"FAIL: {guami_path} not found -- did the C++ round-trip test run first?", file=sys.stderr)
        return 1
    guami_instance = json.loads(guami_path.read_text())
    guami_schema = registry.schemas[("TS29571_CommonData.yaml", "Guami")]
    all_errors.extend(
        check_object(guami_schema, guami_instance, registry, "TS29571_CommonData.yaml", "Guami")
    )

    # NFType: anyOf[{enum:[...]}, {type: string}] means ANY string is schema-valid
    # by design (3GPP's open/extensible enum pattern) -- so the "unknown value"
    # sample is expected to be schema-conformant, not a violation. This is the
    # exact case openapi-generator's generated model could not even represent
    # (see ADR-0010); asserting it round-trips *and* is schema-valid is the point.
    nftype_path = samples_dir / "nftype_unknown.json"
    if not nftype_path.exists():
        print(f"FAIL: {nftype_path} not found", file=sys.stderr)
        return 1
    nftype_value = json.loads(nftype_path.read_text())
    nftype_schema = registry.schemas[("TS29510_Nnrf_NFManagement.yaml", "NFType")]
    any_of = nftype_schema.get("anyOf", [])
    open_string_branch = any(m.get("type") == "string" and "enum" not in m for m in any_of)
    if not (open_string_branch and isinstance(nftype_value, str)):
        all_errors.append("NFType: expected an anyOf[enum,string] schema accepting any string")

    # ADR-0491: the SEPP's N32-c bodies as the SEPP builds them (tests/conformance/test_sepp_n32.cpp),
    # request AND response, against TS29573_N32_Handshake.yaml.
    registry.load_file("TS29573_N32_Handshake.yaml")
    registry.load_file("TS29573_SeppTelescopicFqdnMapping.yaml")
    registry.load_file("TS29573_JOSEProtectedMessageForwarding.yaml")
    for sample, yaml_file, schema_name in (
            ("sec_negotiate_req.json", "TS29573_N32_Handshake.yaml", "SecNegotiateReqData"),
            ("sec_negotiate_rsp.json", "TS29573_N32_Handshake.yaml", "SecNegotiateRspData"),
            # ADR-0493: the PRINS parameter exchange.
            ("sec_param_exch_req.json", "TS29573_N32_Handshake.yaml", "SecParamExchReqData"),
            ("sec_param_exch_rsp.json", "TS29573_N32_Handshake.yaml", "SecParamExchRspData"),
            # ADR-0494: the PRINS N32-f request body.
            ("n32f_reformatted_req.json", "TS29573_JOSEProtectedMessageForwarding.yaml",
             "N32fReformattedReqMsg"),
            # ADR-0492: both TelescopicMapping forms (5.4.2 / 5.4.3).
            ("telescopic_mapping_label.json", "TS29573_SeppTelescopicFqdnMapping.yaml",
             "TelescopicMapping"),
            ("telescopic_mapping_fqdn.json", "TS29573_SeppTelescopicFqdnMapping.yaml",
             "TelescopicMapping")):
        sample_path = samples_dir / sample
        if not sample_path.exists():
            print(f"FAIL: {sample_path} not found", file=sys.stderr)
            return 1
        schema = registry.schemas[(yaml_file, schema_name)]
        all_errors.extend(
            check_object(schema, json.loads(sample_path.read_text()), registry,
                         yaml_file, schema_name)
        )

    # ADR-0519: the NEF's Npcf_PolicyAuthorization create and update bodies
    # (tests/conformance/test_nef_qos_mapping.cpp).
    registry.load_file("TS29514_Npcf_PolicyAuthorization.yaml")
    for sample, schema_name in (("nef_pcf_app_session_req.json", "AppSessionContextReqData"),
                                ("nef_pcf_app_session_update.json", "AppSessionContextUpdateDataPatch")):
        sample_path = samples_dir / sample
        if not sample_path.exists():
            print(f"FAIL: {sample_path} not found", file=sys.stderr)
            return 1
        schema = registry.schemas[("TS29514_Npcf_PolicyAuthorization.yaml", schema_name)]
        all_errors.extend(
            check_object(schema, json.loads(sample_path.read_text()), registry,
                         "TS29514_Npcf_PolicyAuthorization.yaml", schema_name)
        )

    # ADR-0520: the NEF's notification to the AF (tests/conformance/test_nef_qos_mapping.cpp).
    registry.load_file("TS29122_AsSessionWithQoS.yaml")
    sample_path = samples_dir / "nef_af_user_plane_notification.json"
    if not sample_path.exists():
        print(f"FAIL: {sample_path} not found", file=sys.stderr)
        return 1
    all_errors.extend(
        check_object(registry.schemas[("TS29122_AsSessionWithQoS.yaml", "UserPlaneNotificationData")],
                     json.loads(sample_path.read_text()), registry,
                     "TS29122_AsSessionWithQoS.yaml", "UserPlaneNotificationData")
    )

    if all_errors:
        print("FAIL: structural conformance violations:", file=sys.stderr)
        for e in all_errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print("PASS: all samples structurally conform to their specs/5G_APIs-REL-19 schemas")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
