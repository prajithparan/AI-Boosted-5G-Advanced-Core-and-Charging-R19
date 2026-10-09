# SAGE bring-your-own-package framework (design, 2026-10-09)

Status: DESIGN ONLY. Nothing here is implemented. Reason for the shape: the SAGE UEA2/UIA2 documents
and test data are licence-restricted (Restricted Usage Undertaking), so the Apache-2.0 repo must never
contain them, nor code derived from them. The repo ships the *framework*; each licensed operator
supplies the *package* locally and builds the algorithm on their own machine.

## 1. Principles
1. Zero restricted material in git, CI artefacts, container images or logs. Enforced, not promised (section 7).
2. Fail closed: no verified provider -> the AMF does not offer 128-NEA1/NIA1 (it keeps NEA0/NEA2/NIA2, as today).
3. Nothing is "SNOW 3G conformant" until the operator's own build passes the Document 3/4 known-answer vectors (a gate, not documentation).
4. No algorithm detail is written from memory. Every generated line traces to a package clause (section 5).

## 2. Moving parts
```
operator workstation / build host                         repo (public, Apache-2.0)
  sage-package/ (untracked, licensed)  --+                 tools/sage-import/   (importer + templates; NO tables, NO algorithm)
        Doc1-4 (pdf/docx/zip, as issued) |                 libs/nas-alg-provider/ (stable C ABI + loader + selftest gate)
                                          v                 nfs/amf (consumes the ABI only)
              sage-import  --> generated/ (untracked) --> build --> libsnow3g_sage.so (+ .manifest.json, signed)
                                                                      |
                   deployment: config nas_security.alg_providers[] ---+--> AMF dlopen, hash-pin, selftest, then advertise
```

## 3. Stable provider ABI (`libs/nas-alg-provider/include/nas_alg_provider.h`, C, versioned)
Written by us, contains no SAGE content. Sketch (names are ours; not from any spec):
- `nasalg_abi_version()`; `nasalg_describe(out)` -> {algorithm ids offered: 128-EEA1 / 128-EIA1, key size, provenance string, package id}.
- `nasalg_eea1(key[16], count, bearer, direction, in, out, length_bits)` and `nasalg_eia1(key[16], count, fresh, direction, msg, length_bits, mac[4])` -- parameter sets are NAS-side (TS 33.501 / 33.401 Annex B inputs); the provider alone knows how they map to the primitive.
- `nasalg_selftest(vectors_path)` -> runs the operator's Document 3/4 vectors; returns a pass count and the vector-set id.
- Error codes, no allocation across the boundary, explicit lengths, no globals. In-place and separate buffers both defined.
`libs/nas-alg-provider` also carries the loader (dlopen with a root-owned absolute path, sha256 pin from config, symbol/ABI check) and the same ABI wrapper the AMF calls. Existing NEA2/NIA2 (`libs/aka-crypto`) can later be re-expressed behind the same interface; not required.

## 4. `sage-import` (generator) -- three stages, each independently checkable
1. **Ingest and fingerprint.** Reads the operator's package directory, records file names, sizes and sha256 into `package.lock.json` (local). Refuses unknown/edited files unless `--accept-new-package`. Detects the SAGE document version (expects 1.1, 2006-09-06) and stops on any other.
2. **Extract data, never guess.** Per-document *extractors* are small, reviewable, declarative: "table T, document D, clause C, rows N, element width W". Extracted values land in `generated/sage_data.h`. Every extraction has a machine sanity check derived from structure we may state without the secret text (e.g. a claimed S-box has 256 entries and is a permutation; a test vector set has the expected field count). **Blocker:** the extractors cannot be written until someone with the licensed package shows the real document layout (PDF text, docx tables, or an included C source). We have not seen it; we will not invent a parser.
3. **Emit and build.** Fills templates under `tools/sage-import/templates/` with extracted data and builds `libsnow3g_sage.so`. **Open design fork (needs the package to decide):**
   - *Fork A (preferred if the package includes the reference C source):* the generator wraps the licensed reference code behind the ABI; the repo holds only the wrapper template. Least risk of transcription error.
   - *Fork B (document-only package):* the algorithm logic must be transcribed from the spec text. Then the templates contain control flow written by a person who has read the package, reviewed by a second person, in the *operator's* private fork -- not in this public repo -- with the KAT gate as the correctness check. The public repo then holds only the loader/ABI/importer shell.
   Until the package is seen, the framework cannot promise full automation of stage 3.

## 5. Traceability
`generated/TRACE.json` maps each emitted table/function to {document, version, clause/table number, extractor id, sha256 of the source file}. `sage-import --verify` regenerates and diffs; a hand-edit shows up as a diff. The library manifest embeds the package id + TRACE hash, so an AMF log line can say which package a deployed provider came from.

## 6. Conformance gate and AMF integration
- Operator supplies Document 3/4 vectors in their local vault; `nasalg_selftest` runs them at AMF start (and in `sage-import --test`).
- Config (`config/amf.json`, no hardcoding): `nas_security.alg_providers: [{"path": "...", "sha256": "...", "vectors": "..."}]`, default empty. Empty or any failure -> NEA1/NIA1 not in the supported set; the AMF logs why (never the vectors).
- The AMF's UE-security-capability selection then treats 128-NEA1/NIA1 like any other available pair (selection order stays operator-configurable).
- A CI job without a package runs the loader/ABI tests against a **dummy provider that is explicitly not SNOW 3G** (identity-style, id-marked "TEST-ONLY", refused outside test mode) and asserts fail-closed; it makes no conformance claim.

## 7. Keeping restricted material out (enforced)
- `.gitignore` + pre-commit/CI check: deny `snow3g/vault/`, `generated/`, `*.sage`, and any file whose content matches the package fingerprint list; `scripts/check_no_restricted.py` fails the lint job.
- Docker/Helm: provider and vectors mounted as volumes (read-only), never COPY'd into an image; `.dockerignore` mirrors the gitignore.
- Logs/metrics never print vector or table contents; selftest reports counts and set ids only.
- SBOM/licence notice states the provider is operator-supplied and under the operator's own SAGE licence.

## 8. Plan and what is blocked
| Step | Content | Can start now? |
|---|---|---|
| F1 | ABI header + loader + fail-closed AMF wiring + dummy test provider + restricted-material guard + docs | **Built 2026-10-09** (AMF loads providers but does not yet select NEA1/NIA1; see ADR-0479 status update) |
| F2 | `sage-import` stage 1 (ingest/fingerprint/lock) | **Built 2026-10-09** |
| F3 | Extractors (stage 2) | **No** -- needs the licensed package layout |
| F4 | Emit/build (stage 3), Fork A or B | **No** -- same |
| F5 | KAT gate run with real Document 3/4 vectors | **No** -- operator-side only |
Each step is its own small increment with its own ADR and tests.

## 9. Risks / disclosed unknowns
- Whether the ETSI/GSMA undertaking lets an operator build on a *generated* library and ship it to its own AMFs: operator's legal question, not ours. The framework only avoids redistributing anything.
- The `.so` plug-in boundary adds an attack surface (code loaded into the AMF process): mitigated by root-owned path + sha256 pin, but an operator who can write the config can load code; same trust level as the config itself.
- SNOW 3G in the AMF also needs NAS handling beyond the primitive (key derivation for algorithm id 1 in TS 33.501 Annex A, NEA1/NIA1 selection tests); already covered by the existing algorithm-id plumbing for NEA2/NIA2 but unverified for id 1.
