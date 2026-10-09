# snow3g -- SAGE bring-your-own-package framework (ADR-0479)

This directory holds **no SNOW 3G code and no SAGE material**. The licence-restricted ETSI SAGE
package must be supplied by the operator; see `docs/sources.md` (why) and `docs/framework-design.md`
(the plan).

| Built | Where | State |
|---|---|---|
| F1 provider ABI + fail-closed loader + AMF startup load | `libs/nas-alg-provider`, `nfs/amf/src/main.cpp` | built; 6 loader tests with a TEST-ONLY dummy provider; AMF loads and logs, **does not yet select NEA1/NIA1** |
| F1 restricted-material guard | `scripts/check_no_restricted.py`, CI lint job, `.gitignore`/`.dockerignore` | built; 6 negative tests |
| F2 importer stage 1 (ingest, fingerprint, verify) | `tools/sage-import` | built; 12 tests |
| F3 extractors, F4 emit/build, F5 real known-answer run | -- | blocked on the licensed package |

Operator quick start (stage 1 only): create `snow3g/vault/` (git-ignored), put your package files
there, write `snow3g/vault/package.manifest.json` (format in `tools/sage-import/sage_import.py --help`),
then `python3 tools/sage-import/sage_import.py ingest snow3g/vault` and later `... verify snow3g/vault`.
