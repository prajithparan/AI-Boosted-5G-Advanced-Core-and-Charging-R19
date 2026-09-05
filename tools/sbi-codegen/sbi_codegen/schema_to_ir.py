"""Converts raw OpenAPI schema dicts into the IR (ir.py), pulling in the
transitive closure of $ref'd types (from any file) via a worklist so each
named type is converted exactly once regardless of how many pilot files
reference it.

Types are keyed internally by (source_file, yaml_name) -- not by yaml_name
alone -- because different files can legitimately define different schemas
under the same local name (see loader.py's docstring and
docs/DECISIONS.md ADR-0017). A final disambiguation pass
(Converter._disambiguate) runs once the full transitive closure is known,
assigning every (source_file, yaml_name) key a guaranteed-unique C++ type
name: the plain sanitized name when no other file's schema collides with it
(the overwhelming majority), or that name qualified with a source-file tag
when it does.
"""

from __future__ import annotations

import re

from .ir import AliasType, FieldIR, IRType, ObjectType, OpaqueType, OpenEnumType, TypeRef
from .loader import SchemaRegistry, pure_ref_target

_PRIMITIVE_MAP = {
    "string": "std::string",
    "boolean": "bool",
    "integer": "std::int64_t",
    "number": "double",
}

# C++ keywords (including the alternative-operator spellings like 'and'/'or',
# easy to forget but reserved) that collide with legitimate 3GPP field/schema
# names observed in the R19 YAML.
_CPP_RESERVED = {
    "and", "and_eq", "bitand", "bitor", "class", "compl", "delete", "new",
    "not", "not_eq", "operator", "or", "or_eq", "template", "xor", "xor_eq",
}



def _is_constraint_only_member(member: dict) -> bool:
    """True when an allOf member expresses only a CONSTRAINT and carries no data.

    ADR-0301. Recognises the shapes 3GPP actually uses for mutual exclusivity and conditional
    requirement -- `oneOf`/`anyOf`/`not` whose branches are bare `required` lists (or nested
    combinations of the same), plus a bare top-level `required`. Deliberately conservative: a
    member with `properties`, `$ref`, `type`, `items` or `enum` anywhere in it is NOT
    constraint-only, and still falls through to the opaque path rather than having its data
    silently discarded.
    """
    _CONSTRAINT_KEYS = {"oneOf", "anyOf", "allOf", "not", "required", "description", "title"}
    if not isinstance(member, dict) or not member:
        return False
    if not set(member.keys()) <= _CONSTRAINT_KEYS:
        return False
    # A member that is nothing but a description carries no constraint either; treat it as
    # constraint-only (it contributes no fields, which is the property that matters here).
    for key in ("oneOf", "anyOf", "allOf"):
        for branch in member.get(key, []):
            if not _is_constraint_only_member(branch):
                return False
    nested_not = member.get("not")
    if nested_not is not None and not _is_constraint_only_member(nested_not):
        return False
    return True

def _cpp_field_name(json_name: str) -> str:
    # 3GPP JSON field names are lowerCamelCase but some are legitimately
    # digit-led (5qi, 5gMmCapability -- 5G QoS Identifier et al.), which is
    # not a valid C++ identifier start. Prefix with 'n' (kept lowercase to
    # match the surrounding camelCase convention) rather than reject/guess a
    # different name -- this is purely a C++-identifier-legality shim, the
    # JSON wire name (json_name) is untouched and still what's serialized.
    if json_name[:1].isdigit():
        json_name = "n" + json_name
    if json_name in _CPP_RESERVED:
        return json_name + "_"
    return json_name


def cpp_type_name(schema_name: str) -> str:
    """Sanitizes a 3GPP schema name (e.g. '5GDdnmfInfo', '2G3GLocationArea')
    into a valid C++ type identifier. Digit-led names get an 'N' prefix
    (matching the surrounding PascalCase convention) -- applied consistently
    everywhere a schema name becomes a C++ type name (both at the type's own
    declaration and at every reference to it), so they always agree. The
    lookup key used internally (registry/worklist/result dict) stays the
    original, unsanitized YAML name -- only the emitted C++ identifier
    changes. See docs/DECISIONS.md ADR-0010."""
    if schema_name[:1].isdigit():
        return "N" + schema_name
    if schema_name in _CPP_RESERVED:
        return schema_name + "_"
    return schema_name


def _bare_type_name(type_ref: TypeRef) -> str | None:
    """Unwraps array nesting to find the named-type C++ identifier a TypeRef
    ultimately points at (None for plain primitives)."""
    while type_ref.kind == "array":
        type_ref = type_ref.array_of
    return type_ref.cpp_name if type_ref.kind == "ref" else None


def _is_pattern_only(member: dict) -> bool:
    return set(member.keys()) <= {"pattern"} and "pattern" in member


def _is_opaque_type_ref(type_ref: TypeRef) -> bool:
    """True for a TypeRef that resolves (through any array nesting) to the catch-all
    `nlohmann::json` placeholder `_property_type_ref` emits for an intentionally
    unconstrained schema (e.g. `items: {}`) -- see its own 'not confidently modeled inline'
    fallback below. Used only to detect the real 'abstract base narrowed by concrete allOf
    subtype' pattern (e.g. TS26512_EventExposure.yaml's own `BaseEventCollection.records:
    array, items: {}`, narrowed by `QoEMetricsCollection` et al. to `array of QoEMetricsEvent`)
    -- standard JSON-Schema allOf composition, not a genuine conflict, found while wiring
    Nnef_EventExposure (ADR-0209)."""
    while type_ref.kind == "array":
        type_ref = type_ref.array_of
    return type_ref.kind == "primitive" and type_ref.cpp_name == "nlohmann::json"


def _type_ref_key(type_ref: TypeRef) -> tuple:
    """A hashable, structural-equality key for a TypeRef, usable before collision
    disambiguation has assigned final cpp_names (ref_key -- (source_file, yaml_name) -- is
    already stable at conversion time, unlike cpp_name). Used only to detect genuinely
    conflicting vs. compatible duplicate fields in an allOf merge -- see ADR-0190."""
    if type_ref.kind == "array":
        return ("array", _type_ref_key(type_ref.array_of))
    if type_ref.kind == "ref":
        return ("ref", type_ref.ref_key)
    return ("primitive", type_ref.cpp_name)


def _is_open_enum_anyof(schema: dict) -> list[str] | None:
    any_of = schema.get("anyOf")
    if not isinstance(any_of, list) or len(any_of) != 2:
        return None
    enum_member = next((m for m in any_of if isinstance(m, dict) and "enum" in m), None)
    open_member = next(
        (m for m in any_of if isinstance(m, dict) and "enum" not in m and m.get("type") == "string"),
        None,
    )
    if enum_member is not None and open_member is not None:
        return list(enum_member["enum"])
    return None


def _file_tag(source_file: str) -> str:
    """'TS29510_Nnrf_NFManagement.yaml' -> 'Nnrf_NFManagement'. Used only to
    build a disambiguating suffix for colliding schema names -- derived from
    3GPP's own repo filename convention, not invented. See ADR-0017."""
    stem = source_file[:-5] if source_file.endswith(".yaml") else source_file
    return re.sub(r"^TS\d{5}_", "", stem)


class Converter:
    def __init__(self, registry: SchemaRegistry):
        self.registry = registry
        # Keyed by (source_file, yaml_name) -- see module docstring and
        # ADR-0017 for why yaml_name alone is not a safe key.
        self.result: dict[tuple[str, str], IRType] = {}
        self._worklist: list[tuple[str, dict, str]] = []
        self._queued: set[tuple[str, str]] = set()

    def convert_files(self, filenames: list[str]) -> dict[str, IRType]:
        for filename in filenames:
            self.registry.load_file(filename)
            for (source_file, name), schema in list(self.registry.schemas.items()):
                # Pure-$ref entries (see loader.pure_ref_target / ADR-0024) never get their own
                # IR type -- anything that references this name resolves straight through to the
                # real target via registry.resolve_ref, so generating a spurious standalone type
                # under the local alias name here would be wrong (and, worse, would collide-and-
                # disambiguate against the real type for no reason).
                if source_file == filename and pure_ref_target(schema) is None:
                    self._enqueue(name, schema, source_file)

        while self._worklist:
            name, schema, source_file = self._worklist.pop(0)
            key = (source_file, name)
            if key in self.result:
                continue
            self.result[key] = self._convert_one(name, schema, source_file)

        return self._disambiguate()

    def _disambiguate(self) -> dict[str, IRType]:
        """Assigns every (source_file, yaml_name) key a final, guaranteed-
        unique C++ type name, then patches every IRType.name and every
        TypeRef.cpp_name (via TypeRef.ref_key) to match. A plain
        cpp_type_name(yaml_name) is kept as-is when only one source_file
        defines that name; when multiple files each locally define a
        same-named-but-different schema (real, confirmed cases: see
        loader.py's docstring), every one of them is qualified with a
        source-file tag so all survive as distinct types instead of one
        silently overwriting another. See ADR-0017."""
        keys_by_plain_name: dict[str, list[tuple[str, str]]] = {}
        for source_file, yaml_name in self.result:
            plain = cpp_type_name(yaml_name)
            keys_by_plain_name.setdefault(plain, []).append((source_file, yaml_name))

        final_name: dict[tuple[str, str], str] = {}
        for plain, keys in keys_by_plain_name.items():
            if len(keys) == 1:
                final_name[keys[0]] = plain
            else:
                for key in keys:
                    final_name[key] = f"{plain}_{_file_tag(key[0])}"

        def rewrite_ref(ref: TypeRef) -> None:
            if ref.kind == "array":
                rewrite_ref(ref.array_of)
            elif ref.kind == "ref" and ref.ref_key is not None:
                ref.cpp_name = final_name[ref.ref_key]

        renamed: dict[str, IRType] = {}
        for key, t in self.result.items():
            t.name = final_name[key]
            if isinstance(t, ObjectType):
                for f in t.fields:
                    rewrite_ref(f.type_ref)
            elif isinstance(t, AliasType) and t.element_ref is not None:
                # cpp_underlying was flattened to a string at construction time
                # (before disambiguation could know the element type's final
                # name) -- rewrite the structured ref, then regenerate the
                # string from it so a renamed element type is reflected in
                # BOTH places. See ADR-0044.
                rewrite_ref(t.element_ref)
                t.cpp_underlying = f"std::vector<{_type_ref_to_cpp(t.element_ref)}>"
            renamed[t.name] = t
        return renamed

    def _enqueue(self, name: str, schema: dict, source_file: str) -> None:
        key = (source_file, name)
        if key in self._queued:
            return
        self._queued.add(key)
        self._worklist.append((name, schema, source_file))

    def _resolve_ref_typeref(self, ref: str, from_file: str) -> TypeRef:
        name, schema, source_file = self.registry.resolve_ref(ref, from_file)
        self._enqueue(name, schema, source_file)
        return TypeRef(
            kind="ref", cpp_name=cpp_type_name(name), ref_key=(source_file, name)
        )

    def _property_type_ref(self, prop_schema: dict, from_file: str, context_name: str) -> TypeRef:
        if "$ref" in prop_schema:
            return self._resolve_ref_typeref(prop_schema["$ref"], from_file)

        if prop_schema.get("type") == "array":
            items = prop_schema.get("items", {})
            inner = self._property_type_ref(items, from_file, context_name)
            return TypeRef(kind="array", cpp_name="", array_of=inner)

        ptype = prop_schema.get("type")
        if ptype in _PRIMITIVE_MAP and "enum" not in prop_schema and "anyOf" not in prop_schema:
            return TypeRef(kind="primitive", cpp_name=_PRIMITIVE_MAP[ptype])

        # Inline enum, anyOf, or other composed shape on a property: not
        # confidently modeled inline. Fall back to opaque JSON for this one
        # field rather than guessing -- see ADR-0010.
        return TypeRef(kind="primitive", cpp_name="nlohmann::json")

    def _convert_object_properties(
        self, properties: dict, required: list[str], from_file: str, name: str
    ) -> list[FieldIR]:
        fields: list[FieldIR] = []
        for prop_name, prop_schema in properties.items():
            type_ref = self._property_type_ref(prop_schema, from_file, name)
            field_name = _cpp_field_name(prop_name)
            # A field whose C++ name is identical to its own type's C++ name
            # (observed in the R19 YAML, e.g. field "Snssai" of type Snssai)
            # is legal in isolation but changes name lookup for the rest of
            # the enclosing struct ([-Wchanges-meaning], and a real bug, not
            # just a warning, once other members reference that type). Append
            # '_' to disambiguate -- see ADR-0010.
            if field_name == _bare_type_name(type_ref):
                field_name += "_"
            fields.append(
                FieldIR(
                    json_name=prop_name,
                    cpp_name=field_name,
                    type_ref=type_ref,
                    required=prop_name in required,
                    nullable=bool(prop_schema.get("nullable", False)),
                    description=str(prop_schema.get("description", "")).strip(),
                )
            )
        return fields

    def _convert_one(self, name: str, schema: dict, source_file: str) -> IRType:
        description = str(schema.get("description", "")).strip()
        # From here on, `name` is the sanitized C++ identifier (see
        # cpp_type_name); the caller still keys self.result by the original,
        # unsanitized YAML name, so lookups by $ref name are unaffected.
        name = cpp_type_name(name)

        open_enum_values = _is_open_enum_anyof(schema)
        if open_enum_values is not None and all(isinstance(v, str) for v in open_enum_values):
            return OpenEnumType(
                name=name, source_file=source_file, known_values=open_enum_values, description=description
            )

        if "enum" in schema and all(isinstance(v, str) for v in schema["enum"]):
            return OpenEnumType(
                name=name,
                source_file=source_file,
                known_values=list(schema["enum"]),
                description=description,
            )
        if "enum" in schema:
            # Non-string enum (e.g. boolean/int enum) -- not the 3GPP open-enum
            # pattern this generator targets; fall back rather than guess.
            return OpaqueType(
                name=name,
                source_file=source_file,
                reason=f"non-string enum values: {schema['enum']!r}",
                description=description,
            )

        all_of = schema.get("allOf")
        if isinstance(all_of, list) and all_of and all(_is_pattern_only(m) for m in all_of):
            return AliasType(
                name=name,
                source_file=source_file,
                cpp_underlying="std::string",
                patterns=[m["pattern"] for m in all_of],
                description=description,
            )

        if isinstance(all_of, list) and all_of:
            merged_fields: list[FieldIR] = []
            seen_by_json_name: dict[str, FieldIR] = {}
            index_by_json_name: dict[str, int] = {}
            # ADR-0301: a schema may carry `allOf` AND its own sibling `properties`. JSON Schema
            # composes both -- the effective type is "all of the members AND my own properties" --
            # so the schema's own block is processed as an implicit final member rather than
            # ignored.
            #
            # This was latent until constraint-only members started contributing zero fields
            # (above): before that, every non-opaque allOf type drew all its fields from $ref or
            # object members, so dropping the siblings happened to be invisible. With
            # TS29522_TrafficInfluence's TrafficInfluSub -- whose allOf is PURELY constraints and
            # whose every real field is a sibling -- it produced a struct with no fields at all,
            # which is worse than the opaque fallback it replaced: it looks like a real DTO and
            # would silently drop every field on round-trip. Caught by reading the generated
            # output rather than trusting that "it is a struct now" meant it was correct.
            members = list(all_of)
            own_properties = schema.get("properties")
            if isinstance(own_properties, dict) and own_properties:
                members.append(
                    {
                        "type": "object",
                        "properties": own_properties,
                        "required": schema.get("required", []),
                    }
                )
            for member in members:
                if "$ref" in member:
                    ref_name, ref_schema, ref_file = self.registry.resolve_ref(member["$ref"], source_file)
                    self._enqueue(ref_name, ref_schema, ref_file)
                    props = ref_schema.get("properties", {})
                    req = ref_schema.get("required", [])
                    new_fields = self._convert_object_properties(props, req, ref_file, name)
                elif member.get("type") == "object":
                    props = member.get("properties", {})
                    req = member.get("required", [])
                    new_fields = self._convert_object_properties(props, req, source_file, name)
                elif _is_constraint_only_member(member):
                    # ADR-0301: a CONSTRAINT-only allOf member contributes no fields, so it must
                    # not make the whole type opaque.
                    #
                    # 3GPP uses this shape constantly to express mutual exclusivity, e.g.
                    # TS29522_TrafficInfluence's TrafficInfluSub:
                    #     allOf:
                    #       - oneOf: [required:[afAppId], required:[trafficFilters], ...]
                    #       - oneOf: [required:[ipv4Addr], required:[ipv6Addr], ...]
                    # Every branch is a bare `required` list. There are no properties anywhere in
                    # it -- all the type's real data sits in the sibling `properties:` block this
                    # generator already handles correctly. Treating it as unmodellable discarded a
                    # fully-modellable type.
                    #
                    # What IS genuinely lost, and is disclosed rather than silently dropped: the
                    # generated struct cannot ENFORCE "exactly one of these is present". Every
                    # field simply stays optional. That is a real, narrower gap than an opaque
                    # nlohmann::json (which enforces nothing AND validates no field types), and it
                    # is the same class of runtime-validation gap this generator already has for
                    # ordinary `required` handling.
                    new_fields = []
                else:
                    return OpaqueType(
                        name=name,
                        source_file=source_file,
                        reason=f"allOf member not $ref/object/pattern-only: {list(member.keys())}",
                        description=description,
                    )
                # Real, confirmed case (not hypothetical): two allOf'd parent schemas can each
                # legitimately declare a field with the same JSON name -- e.g.
                # ProblemDetailsProvidePosInfo's own ProblemDetails and ProvidePosInfo parents
                # (TS29518_Namf_Location.yaml) both declare `supportedFeatures`. A naive
                # concatenation (the previous behavior here) emits two same-named C++ struct
                # members, an invalid redeclaration -- found building LMF, ADR-0190. When the
                # duplicate is structurally identical (same type shape, same required/nullable),
                # keep only the first occurrence, matching ordinary JSON-Schema allOf composition
                # semantics for compatible duplicate properties. A genuinely conflicting duplicate
                # (different type/required/nullable) is not silently resolved -- raise rather than
                # emit unverified code, same precedent as the cyclic-required-field check below.
                for f in new_fields:
                    prior = seen_by_json_name.get(f.json_name)
                    if prior is None:
                        seen_by_json_name[f.json_name] = f
                        index_by_json_name[f.json_name] = len(merged_fields)
                        merged_fields.append(f)
                        continue
                    if (
                        _type_ref_key(prior.type_ref) != _type_ref_key(f.type_ref)
                        or prior.required != f.required
                        or prior.nullable != f.nullable
                    ):
                        # Real, confirmed case (not hypothetical, found wiring
                        # Nnef_EventExposure, ADR-0209): an abstract base allOf member
                        # intentionally leaves a field's item type unconstrained (`items: {}`,
                        # converted to the `nlohmann::json` placeholder above) and a later
                        # allOf member narrows it to a specific type -- standard JSON-Schema
                        # allOf composition (the effective type is the more specific one), not
                        # a genuine conflict. required/nullable must still agree; only the type
                        # is allowed to narrow, and only one side may be the opaque placeholder.
                        if (
                            prior.required == f.required
                            and prior.nullable == f.nullable
                            and _is_opaque_type_ref(prior.type_ref) != _is_opaque_type_ref(f.type_ref)
                        ):
                            narrower = f if _is_opaque_type_ref(prior.type_ref) else prior
                            seen_by_json_name[f.json_name] = narrower
                            merged_fields[index_by_json_name[f.json_name]] = narrower
                            continue
                        raise NotImplementedError(
                            f"{name}: allOf members disagree on field '{f.json_name}' "
                            f"(first: {prior.type_ref}, required={prior.required}; "
                            f"second: {f.type_ref}, required={f.required}) -- no codegen "
                            "support for a genuinely conflicting allOf-duplicate field, see "
                            "ADR-0190. Not silently emitting one arbitrary side."
                        )
            return ObjectType(name=name, source_file=source_file, fields=merged_fields, description=description)

        if schema.get("type") == "object" or "properties" in schema:
            properties = schema.get("properties", {})
            required = schema.get("required", [])
            fields = self._convert_object_properties(properties, required, source_file, name)
            return ObjectType(name=name, source_file=source_file, fields=fields, description=description)

        if schema.get("type") == "array":
            items = schema.get("items", {})
            inner = self._property_type_ref(items, source_file, name)
            inner_cpp = _type_ref_to_cpp(inner)
            # element_ref is kept (not just the flattened string) so a later
            # collision rename of the element type can be reflected here too
            # -- see AliasType's docstring and ADR-0044. Kept for "primitive"
            # kind too (harmless -- rewrite_ref only acts on "ref"/"array").
            return AliasType(
                name=name,
                source_file=source_file,
                cpp_underlying=f"std::vector<{inner_cpp}>",
                description=description,
                element_ref=inner,
            )

        ptype = schema.get("type")
        if ptype in _PRIMITIVE_MAP:
            patterns = []
            if "pattern" in schema:
                patterns.append(schema["pattern"])
            return AliasType(
                name=name,
                source_file=source_file,
                cpp_underlying=_PRIMITIVE_MAP[ptype],
                patterns=patterns,
                description=description,
            )

        return OpaqueType(
            name=name,
            source_file=source_file,
            reason=f"unhandled schema shape, keys={list(schema.keys())}",
            description=description,
        )


def _type_ref_to_cpp(ref: TypeRef) -> str:
    if ref.kind == "array":
        return f"std::vector<{_type_ref_to_cpp(ref.array_of)}>"
    return ref.cpp_name
