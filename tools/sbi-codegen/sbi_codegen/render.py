"""Renders the IR (ir.py) into C++ headers/sources via Jinja2 templates.

3GPP's OpenAPI files have genuine cross-file circular dependencies (e.g.
TS29571_CommonData.yaml pulls in TS29510_Nnrf_AccessToken.yaml, which
(transitively) needs types back from CommonData -- confirmed by compiling a
first, naive one-header-per-source-file version and hitting "not declared in
this scope" from #pragma once silently no-op'ing a re-entrant #include before
the needed type was defined). So files are first grouped into strongly
connected components (Tarjan's algorithm) and each SCC is emitted as ONE
header/source pair, with the types *within* it topologically sorted by
field-level dependency so no forward declarations are needed. See
docs/DECISIONS.md ADR-0010.
"""

from __future__ import annotations

import pathlib
import re

import jinja2

from .ir import AliasType, ObjectType, OpaqueType, OpenEnumType, TypeRef

_TEMPLATES_DIR = pathlib.Path(__file__).parent.parent / "templates"



# ADR-0313: how many types' to_json/from_json bodies go into one generated .cpp.
#
# Measured on this machine, not guessed:
#   unsplit (one ~3,000-type file):  10.0 GB peak RSS, 4m45s
#   400 types/part (7 parts):         5.1 GB peak RSS, 1m40s
#   150 types/part:                   see below -- chosen so several parts compile CONCURRENTLY
#
# The target is not just "smaller"; it is that `-j2` or better fits in this 15 GB machine while
# other work runs. At 5.1 GB two concurrent parts would need 10.2 GB and still risk the OOM killer,
# which is the exact failure this change exists to remove. 150 keeps each part low enough that
# parallel compilation is genuinely safe again, at the cost of more object files -- link input is
# cheap, OOM kills are not.
_MAX_TYPES_PER_SOURCE = 150


def _partition_for_sources(object_types, open_enum_types, alias_types):
    """Deal the type lists across N source-file contexts, preserving order.

    The header declares every type; each part implements a slice. Order is preserved so a type's
    implementation stays in the same part across regenerations unless the schema itself changes.
    """
    total = len(object_types) + len(open_enum_types) + len(alias_types)
    if total <= _MAX_TYPES_PER_SOURCE:
        return [
            {
                "object_types": object_types,
                "open_enum_types": open_enum_types,
                "alias_types": alias_types,
            }
        ]

    parts: list[dict] = []
    # Aliases first (they carry the pattern validators and are cheap), then enums, then objects --
    # objects dominate the count and the compile cost, so they are what actually gets spread.
    remaining_alias = list(alias_types)
    remaining_enum = list(open_enum_types)
    remaining_object = list(object_types)

    while remaining_alias or remaining_enum or remaining_object:
        budget = _MAX_TYPES_PER_SOURCE
        take_alias, remaining_alias = remaining_alias[:budget], remaining_alias[budget:]
        budget -= len(take_alias)
        take_enum, remaining_enum = remaining_enum[:budget], remaining_enum[budget:]
        budget -= len(take_enum)
        take_object, remaining_object = remaining_object[:budget], remaining_object[budget:]
        parts.append(
            {
                "object_types": take_object,
                "open_enum_types": take_enum,
                "alias_types": take_alias,
            }
        )
    return parts

def _type_ref_to_cpp(ref: TypeRef) -> str:
    if ref.kind == "array":
        return f"std::vector<{_type_ref_to_cpp(ref.array_of)}>"
    return ref.cpp_name


def _enumconst(value: str) -> str:
    ident = re.sub(r"[^A-Za-z0-9_]", "_", value)
    if ident[:1].isdigit():
        ident = "V" + ident
    return ident


def _cpp_comment(text: str) -> str:
    """Prefixes every line of a (possibly multi-line) description with '// ',
    so multi-paragraph 3GPP YAML prose can never leak un-commented raw text
    (with embedded quotes/decimal-looking clause numbers like '23.003') into
    the generated file as bare code. A first pass of this template emitted
    only the first line commented, which does not compile -- see ADR-0010."""
    lines = text.replace("\r\n", "\n").split("\n")
    return "\n".join(f"// {line}".rstrip() for line in lines)


def ts_number_from_filename(stem: str) -> str:
    """'TS29571_CommonData' -> 'TS 29.571'. Derived from the filename, not
    invented -- 3GPP's own repo naming convention encodes the TS number."""
    m = re.match(r"TS(\d{5})_", stem)
    if not m:
        return stem
    digits = m.group(1)
    return f"TS {digits[:2]}.{digits[2:]}"


class RenderField:
    def __init__(self, f, optional: bool, needs_indirection: bool = False):
        self.json_name = f.json_name
        self.cpp_name = f.cpp_name
        self.optional = optional
        inner = _type_ref_to_cpp(f.type_ref)
        if needs_indirection:
            # Real cycle in 3GPP's own schema (see _topo_sort_types' own docstring for the
            # confirmed example: SharedData.sharedAmData -> AccessAndMobilitySubscriptionData ->
            # AccessAndMobilitySubscriptionData.sharedDataList -> SharedData). This field's
            # referenced type is only forward-declared, not yet complete, at this point in
            # emission order -- direct std::optional<T>/std::vector<T> embedding needs T
            # complete (both for their own destructor and, for vector, essentially every other
            # operation too), which a forward declaration alone can never satisfy for a genuine
            # cycle, no matter which emission order is chosen. std::shared_ptr<T> does not need
            # T complete at this point: its deleter is type-erased at construction time (in the
            # generated .cpp file, where T IS complete by then), not at the point the containing
            # struct's implicitly-defaulted destructor is instantiated here in the header. See
            # docs/DECISIONS.md ADR-0052.
            self.cpp_type = f"std::shared_ptr<{inner}>"
        else:
            self.cpp_type = f"std::optional<{inner}>" if optional else inner


_IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _referenced_names(t) -> list[str]:
    """Named types directly referenced by t (for dependency graphs, both
    file-level and type-level).

    AliasType's element_ref (set only for the array-of-named-type case, see
    ir.py/ADR-0044) is a real structured TypeRef, walked the same way as an
    ObjectType field -- this is what makes it visible to
    Converter._disambiguate's rename pass in the first place, so it must also
    be what dependency tracking here uses; a plain string is never
    correct after a collision rename. See docs/DECISIONS.md ADR-0022 for why
    file-level dependency tracking on alias underlying types matters at all:
    without it, an alias whose underlying type names a struct/enum defined
    later in the same merged group compiles with "not declared in this
    scope", since header.hpp.j2 always emits the alias block before the
    enum/object blocks regardless of computed order.
    """
    names: list[str] = []

    def walk(ref: TypeRef) -> None:
        if ref.kind == "array":
            walk(ref.array_of)
        elif ref.kind == "ref":
            names.append(ref.cpp_name)

    if isinstance(t, ObjectType):
        for f in t.fields:
            walk(f.type_ref)
    elif isinstance(t, AliasType):
        if t.element_ref is not None:
            walk(t.element_ref)
        else:
            names.extend(_IDENTIFIER_RE.findall(t.cpp_underlying))
    return names


def _tarjan_scc(nodes: list[str], edges: dict[str, set[str]]) -> list[list[str]]:
    """Standard Tarjan's algorithm, iterative order preserved via index/lowlink."""
    index_counter = [0]
    stack: list[str] = []
    on_stack: set[str] = set()
    indices: dict[str, int] = {}
    lowlink: dict[str, int] = {}
    result: list[list[str]] = []

    def strongconnect(v: str) -> None:
        indices[v] = index_counter[0]
        lowlink[v] = index_counter[0]
        index_counter[0] += 1
        stack.append(v)
        on_stack.add(v)

        for w in edges.get(v, ()):
            if w not in indices:
                strongconnect(w)
                lowlink[v] = min(lowlink[v], lowlink[w])
            elif w in on_stack:
                lowlink[v] = min(lowlink[v], indices[w])

        if lowlink[v] == indices[v]:
            component = []
            while True:
                w = stack.pop()
                on_stack.discard(w)
                component.append(w)
                if w == v:
                    break
            result.append(component)

    import sys

    old_limit = sys.getrecursionlimit()
    sys.setrecursionlimit(max(old_limit, 10000))
    try:
        for v in nodes:
            if v not in indices:
                strongconnect(v)
    finally:
        sys.setrecursionlimit(old_limit)

    return result


def _forward_only_ref_name(
    type_ref: TypeRef, container_position: int, position: dict, cyclic_names: set
) -> "str | None":
    """If type_ref (possibly array-of) references a same-group type that participates in a
    real cycle (see _topo_sort_types' docstring) AND is not yet fully defined at
    container_position (this field's own containing type's position in emission order), returns
    that referenced type's name -- the field needs std::shared_ptr<T> indirection instead of
    direct embedding, see RenderField's own comment. Returns None otherwise (including for a
    cyclic-group type that DOES already have a full definition earlier in emission order --
    only one side of a 2-cycle actually needs the indirection, whichever is emitted second gets
    to reference the first directly)."""
    if type_ref.kind == "array":
        return _forward_only_ref_name(type_ref.array_of, container_position, position, cyclic_names)
    if type_ref.kind == "ref":
        name = type_ref.cpp_name
        if name in cyclic_names and name in position and position[name] > container_position:
            return name
    return None


def _topo_sort_types(
    names: list[str], types_by_name: dict, all_names_in_group: set[str]
) -> tuple[list[str], set[str]]:
    """Topologically sorts types within a group by field-level dependency (only
    counting deps on other types in the same group -- cross-group deps are
    #includes, already satisfied).

    Real, genuine cycles exist in 3GPP's own schemas within a single merged
    group (found compiling the R19 pilot files once TS29503_Nudm_SDM.yaml was
    added: SharedData.sharedAmData -> AccessAndMobilitySubscriptionData ->
    AccessAndMobilitySubscriptionData.sharedDataList -> SharedData, a real
    "shared data aggregates per-type subscription data, per-type subscription
    data can itself be shared" pattern -- not a generator artifact). A prior
    version of this function fell back to raw input order for the WHOLE group
    the moment ANY cycle was found anywhere in it -- which, since a single
    2-type cycle poisons the traversal, silently discarded the correct
    ordering for potentially hundreds of unrelated, perfectly acyclic types in
    the same group too, producing "used before declared" compile errors far
    from the actual cycle. See docs/DECISIONS.md ADR-0022.

    Fixed properly via SCC condensation: strongly-connected components (each a
    genuine cycle, or a lone acyclic type) are computed via Tarjan's
    algorithm, the condensation graph over those components is always a DAG
    (SCCs cannot cycle with each other by construction) and is topologically
    sorted normally, and only the members of a real multi-type SCC are
    reported back as needing a forward declaration (emitted by the caller
    before any of that SCC's members are defined) -- everything outside an
    actual cycle still gets a fully correct dependency order. Only ObjectType
    nodes can have outgoing edges (see _referenced_names), so a multi-member
    SCC can only ever consist of ObjectTypes -- forward-declaring `struct X;`
    is always the right (and only) declaration shape needed here.

    Returns (ordered_names, cyclic_names): cyclic_names is the set of type
    names that participate in a real (size > 1) cycle and need a forward
    declaration before their first use.
    """
    edges = {n: set() for n in names}
    for n in names:
        for dep in _referenced_names(types_by_name[n]):
            if dep in all_names_in_group and dep != n:
                edges[n].add(dep)

    sccs = _tarjan_scc(names, edges)

    scc_index: dict[str, int] = {}
    for i, scc in enumerate(sccs):
        for n in scc:
            scc_index[n] = i

    condensed_edges: dict[int, set[int]] = {i: set() for i in range(len(sccs))}
    for n in names:
        for dep in edges[n]:
            if scc_index[dep] != scc_index[n]:
                condensed_edges[scc_index[n]].add(scc_index[dep])

    order_indices: list[int] = []
    visited: set[int] = set()
    temp_mark: set[int] = set()

    def visit(i: int) -> None:
        if i in visited:
            return
        temp_mark.add(i)
        for dep_i in condensed_edges[i]:
            visit(dep_i)
        temp_mark.discard(i)
        visited.add(i)
        order_indices.append(i)

    for i in range(len(sccs)):
        visit(i)

    ordered_names: list[str] = []
    cyclic_names: set[str] = set()
    for i in order_indices:
        scc = sccs[i]
        if len(scc) > 1:
            cyclic_names.update(scc)
        # Preserve original relative order within an SCC for deterministic output.
        ordered_names.extend(n for n in names if n in scc)

    return ordered_names, cyclic_names


def render(ir_types: dict, commit: str, out_dir: pathlib.Path) -> list[pathlib.Path]:
    env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(str(_TEMPLATES_DIR)),
        trim_blocks=True,
        lstrip_blocks=True,
    )
    env.filters["enumconst"] = _enumconst
    env.filters["cppcomment"] = _cpp_comment

    name_to_file = {t.name: t.source_file for t in ir_types.values()}
    name_to_type = {t.name: t for t in ir_types.values()}

    by_file: dict[str, list[str]] = {}
    for t in ir_types.values():
        by_file.setdefault(t.source_file, []).append(t.name)

    file_stems = {f: pathlib.Path(f).stem for f in by_file}

    # File-level dependency graph (for SCC / cycle detection).
    file_edges: dict[str, set[str]] = {stem: set() for stem in file_stems.values()}
    for source_file, names in by_file.items():
        self_stem = file_stems[source_file]
        for n in names:
            for dep_name in _referenced_names(name_to_type[n]):
                dep_file = name_to_file.get(dep_name)
                if dep_file is not None:
                    dep_stem = file_stems[dep_file]
                    if dep_stem != self_stem:
                        file_edges[self_stem].add(dep_stem)

    sccs = _tarjan_scc(list(file_stems.values()), file_edges)

    stem_to_group_id: dict[str, int] = {}
    groups: list[dict] = []
    for i, scc in enumerate(sccs):
        for stem in scc:
            stem_to_group_id[stem] = i
        groups.append({"stems": scc, "types": [], "source_files": []})

    stem_to_source_file = {v: k for k, v in file_stems.items()}
    for source_file, names in by_file.items():
        gid = stem_to_group_id[file_stems[source_file]]
        groups[gid]["types"].extend(names)
        groups[gid]["source_files"].append(source_file)

    written: list[pathlib.Path] = []
    out_dir.mkdir(parents=True, exist_ok=True)

    group_name_for_stem: dict[str, str] = {}
    for group in groups:
        stems_sorted = sorted(group["stems"])
        group_name = stems_sorted[0] if len(stems_sorted) == 1 else stems_sorted[0] + "_grp"
        group["name"] = group_name
        for stem in group["stems"]:
            group_name_for_stem[stem] = group_name

    for group in groups:
        group_name = group["name"]
        all_names_in_group = set(group["types"])
        ordered_names, cyclic_names = _topo_sort_types(
            group["types"], name_to_type, all_names_in_group
        )
        position = {n: i for i, n in enumerate(ordered_names)}

        deps: set[str] = set()
        object_types = []
        open_enum_types = []
        alias_types = []
        opaque_types = []
        # header.hpp.j2 always emits opaque, then alias, then open-enum, then object blocks in
        # that fixed order, regardless of ordered_names' computed interleaving (only the relative
        # order *within* each block follows ordered_names). So any AliasType that depends on a
        # struct/enum type in this same group needs a forward declaration -- that dependency can
        # never otherwise be satisfied by emission order alone, cyclic or not. See ADR-0022.
        alias_forward_decls: set[str] = set()

        for n in ordered_names:
            t = name_to_type[n]
            # Cross-group #include tracking applies to every kind that can reference a named
            # type, not just ObjectType -- AliasType's cpp_underlying can too (e.g. `using X =
            # std::vector<Y>` where Y lives in another merged group).
            for dep_name in _referenced_names(t):
                dep_file = name_to_file.get(dep_name)
                if dep_file is not None:
                    dep_stem = file_stems[dep_file]
                    dep_group = group_name_for_stem[dep_stem]
                    if dep_group != group_name:
                        deps.add(dep_group)

            if isinstance(t, ObjectType):
                render_fields = []
                this_position = position[n]
                for f in t.fields:
                    optional = (not f.required) or f.nullable
                    forward_only = _forward_only_ref_name(
                        f.type_ref, this_position, position, cyclic_names
                    )
                    if forward_only is not None and not optional:
                        # Not yet a real case in the R19 corpus (both known instances of this
                        # cycle -- SharedData.sharedAmData and AccessAndMobilitySubscriptionData.
                        # sharedDataList -- are optional in the real schema). A REQUIRED field
                        # that's also a cyclic back-edge would need real, different (de)serialize
                        # codegen (std::shared_ptr<T> but enforcing presence like a required
                        # field, not silently-absent like an optional one) that doesn't exist yet
                        # -- stop and ask rather than silently emit something never exercised.
                        raise NotImplementedError(
                            f"{t.name}.{f.cpp_name}: required field is a cyclic-schema "
                            "back-edge (references forward-declared-only type "
                            f"'{forward_only}') -- no codegen support for this combination yet, "
                            "see RenderField's ADR-0052 comment. Not silently generating "
                            "unverified code for it."
                        )
                    render_fields.append(
                        RenderField(f, optional, needs_indirection=forward_only is not None)
                    )
                object_types.append((t, render_fields))
            elif isinstance(t, OpenEnumType):
                open_enum_types.append(t)
            elif isinstance(t, AliasType):
                alias_types.append(t)
                for dep_name in _referenced_names(t):
                    if dep_name in all_names_in_group:
                        dep_type = name_to_type[dep_name]
                        if isinstance(dep_type, (ObjectType, OpenEnumType)):
                            alias_forward_decls.add(dep_name)
            elif isinstance(t, OpaqueType):
                opaque_types.append(t)

        # Deterministic emission order for the forward-declaration block -- see
        # _topo_sort_types' docstring and ADR-0022.
        forward_declared_types = sorted(cyclic_names | alias_forward_decls)

        citations = [
            (ts_number_from_filename(pathlib.Path(sf).stem), sf) for sf in sorted(group["source_files"])
        ]

        ctx = {
            "stem": group_name,
            "citations": citations,
            "commit": commit,
            "deps": sorted(deps),
            "forward_declared_types": forward_declared_types,
            "object_types": object_types,
            "open_enum_types": open_enum_types,
            "alias_types": alias_types,
            "opaque_types": opaque_types,
        }

        hpp = env.get_template("header.hpp.j2").render(**ctx)
        hpp_path = out_dir / f"{group_name}.hpp"
        hpp_path.write_text(hpp, encoding="utf-8")
        written.append(hpp_path)

        # ADR-0313: the HEADER stays whole, the SOURCE is split across translation units.
        #
        # The monolithic header is not an accident and must not be split: 3GPP's YAML files have
        # genuine cross-file circular dependencies, so this generator groups them into strongly
        # connected components and emits one header per SCC with the types inside it topologically
        # sorted (ADR-0010). Splitting the header would reintroduce exactly the "not declared in
        # this scope" failure that design exists to prevent.
        #
        # The IMPLEMENTATION has no such constraint. Every to_json/from_json body is independent
        # once the header is included, so the function bodies can be dealt out across several .cpp
        # files that each include the same whole header.
        #
        # Why this matters, measured rather than assumed: compiling the single 33,263-line
        # TS26510_CommonData_grp.cpp peaked at **10.0 GB RSS over 4m45s** (/usr/bin/time -v, this
        # machine, after all 60 AF-facing files were wired in ADR-0312). That one translation unit
        # is why a 15 GB machine could not sustain even -j2 and produced ten OOM kills in a single
        # session. Splitting it into parts of at most _MAX_TYPES_PER_SOURCE types brings each part
        # back to roughly a gigabyte.
        #
        # Types are partitioned in the SAME deterministic order the header declares them, so a
        # given type's implementation lands in a stable file across runs -- a rebuild does not
        # reshuffle every part and invalidate every object file.
        parts = _partition_for_sources(object_types, open_enum_types, alias_types)
        for index, part in enumerate(parts):
            part_ctx = dict(ctx)
            part_ctx.update(part)
            # Only the first part emits the opaque-type bodies (there are none to emit) and any
            # future whole-group preamble; keeping that on one part avoids duplicate definitions.
            cpp = env.get_template("source.cpp.j2").render(**part_ctx)
            suffix = "" if len(parts) == 1 else f"_part{index + 1}"
            cpp_path = out_dir / f"{group_name}{suffix}.cpp"
            cpp_path.write_text(cpp, encoding="utf-8")
            written.append(cpp_path)

    return written
