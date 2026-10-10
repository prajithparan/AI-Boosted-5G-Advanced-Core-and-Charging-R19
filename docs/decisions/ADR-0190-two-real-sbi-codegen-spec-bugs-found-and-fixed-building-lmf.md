## ADR-0190: two real sbi-codegen/spec bugs found and fixed building LMF

### Context

Building LMF (fourth Tier 2 NF, this project's sixteenth) required pulling in
`TS29572_Nlmf_Location.yaml` plus its own cross-file dependencies
(`TS29518_Namf_Location.yaml`, `TS29518_Namf_EventExposure.yaml`) into the codegen pilot set for
the first time. Doing so surfaced two real, previously-latent defects -- one in the vendored spec
snapshot, one in this project's own generator -- neither hypothetical, both confirmed by direct
read of the source before touching anything, per CLAUDE.md's own "never invent, verify first" rule.

### Bug 1: real transcription typo in the vendored spec snapshot

`specs/5G_APIs-REL-19/TS29518_Namf_EventExposure.yaml:1679`'s `FailureReason` enum declared
`SECURITY_MODE_REJECT"` -- a literal stray double-quote character as part of the YAML scalar
itself (not YAML syntax), breaking the generated C++ string literal
(`static inline const std::string SECURITY_MODE_REJECT_ = "SECURITY_MODE_REJECT"";`, a compile
error). Confirmed via direct read of the raw YAML line -- an unambiguous transcription defect (no
real 3GPP enum value contains a literal quote character), not a naming ambiguity requiring a
decision. Fixed by removing the single stray character from the vendored file, restoring the real
value `SECURITY_MODE_REJECT`; no field/value was invented, one corrupted character was removed.

### Bug 2: real sbi-codegen allOf-merge bug -- no field deduplication

`ProblemDetailsProvidePosInfo` (`TS29518_Namf_Location.yaml`) is `allOf`
[`ProblemDetails` (`TS29571_CommonData.yaml`), `ProvidePosInfo` (local)]. Both parent schemas
genuinely, independently declare a `supportedFeatures` field (confirmed by direct read of both --
`ProvidePosInfo` is a much larger schema than an earlier partial read suggested, and its own
`supportedFeatures` at line 419-420 is real, not a misread). `tools/sbi-codegen/sbi_codegen/
schema_to_ir.py`'s `_convert_one`'s `allOf` handling naively concatenated fields from every member
with no deduplication by JSON name, producing an invalid duplicate C++ struct member declaration
-- a real generator limitation, not a spec defect, and the third real sbi-codegen bug found this
way (after ADR-0022's topo-sort-on-cycle bug and ADR-0024's pure-$ref-reexport bug).

**Fix**: `schema_to_ir.py`'s `allOf` merge now tracks fields by JSON name as it merges each member.
A duplicate that's structurally identical (same `TypeRef` shape via a new `_type_ref_key` helper,
same `required`/`nullable`) is deduplicated, keeping the first occurrence -- matching ordinary
JSON-Schema `allOf` composition semantics for compatible duplicate properties. A duplicate that
genuinely conflicts (different type/required/nullable) is NOT silently resolved -- raises
`NotImplementedError` at generation time, same "stop rather than emit unverified code" precedent
already used for the cyclic-required-field case in the same function. No real conflicting case has
been observed in the R19 corpus yet; this is defensive, not dead code -- if one is ever hit, it
will fail loudly at generation time, not silently emit wrong C++.

**Real, disclosed process incident during this fix**: `clang-format-18` was accidentally run
against `schema_to_ir.py` (a Python file) alongside the C++ files being formatted in the same
command, corrupting its syntax (converted Python comments/docstrings into invalid partial C++-
style formatting). Caught immediately via `python3 -c "import ast; ast.parse(...)"` failing to
parse the mangled file; fixed by restoring the pre-edit file from `git show HEAD:...` and
reapplying the two real edits above cleanly, then re-verified both the Python syntax and that
codegen still produces the correct fix. No incorrect code was ever committed.

### Testing

`python3 -c "import ast; ast.parse(...)"` confirms valid Python syntax. Codegen re-run standalone
(`TS29572_Nlmf_Location.yaml` + its real cross-file dependencies) confirms the `ProblemDetails
ProvidePosInfo` struct now declares `supportedFeatures` exactly once and the `SECURITY_MODE_REJECT`
string literal is well-formed. Full project rebuild (82/82 steps) and full `ctest` (excluding the
two disclosed pre-existing flaky tests, see ADR-0189 for the LMF-specific 361/361 count this same
rebuild produced) both clean.

### What this ADR does NOT include

A general audit of the rest of the R19 corpus for similar `allOf`-duplicate-field cases or other
transcription typos -- both were found reactively, by hitting them building a specific NF, not via
a proactive sweep. Real, disclosed: more may exist elsewhere in the ~500-file corpus, undiscovered
until a future NF's own pilot-file addition surfaces them, same pattern as ADR-0022/ADR-0024.

