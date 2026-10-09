# SNOW 3G / 128-EEA1 / 128-EIA1 -- source audit (Phase 0, 2026-10-09)

**Result: BLOCKED. No code has been written.** The normative algorithm text and test vectors are
not in any material available to this project.

| Reference | Version | What it contains | Technical content for implementation? |
|---|---|---|---|
| 3GPP TS 35.215 (UEA2/UIA2 Document 1) | Rel-19 j00, fetched with `tools/specs/fetch_3gpp_specs.py` | ZIP with one .docx, 195 lines: scope/foreword only | **No.** "The technical provisions ... are contained in the SAGE Specification" |
| 3GPP TS 35.216 (SNOW 3G, Document 2) | Rel-19 j00 | same cover-page structure | **No.** Same sentence |
| 3GPP TS 35.217 (implementors' test data) | Rel-19 j00 | same | **No** vectors in the file |
| 3GPP TS 35.218 (design and evaluation) | Rel-19 j00 | same | **No** |
| ETSI SAGE UEA2 & UIA2, Documents 1-3 | v1.1, 2006-09-06 (named in TS 35.215/216 refs) | The actual spec: S-boxes, LFSR/FSM, f8/f9, test sets | **Not obtained.** https://www.etsi.org/expertise/algorithms-codes/ states: "customers are required to sign a Restricted Usage Undertaking and pay an associated administrative fee" (details via GSMA). Not downloadable directly. |
| TS 33.401 Annex B / TS 33.501 5.11, D | -- | How 128-EEA1/EIA1 are keyed (COUNT/BEARER/DIRECTION/FRESH inputs) -- they delegate to the SAGE docs | Not read yet; only meaningful together with the SAGE text |
| Open5GS `lib/crypt/snow-3g.c` | -- | An implementation | **Not used.** Instructed as behavioural study only; its licence is not verified here and the project rules forbid copying/translating it. Its tables must not be transcribed into this tree. |

## Established vs unverified
- Established: SNOW 3G is the algorithm behind 128-EEA1/EIA1 (UEA2/UIA2); the AMF "shall" support them (TS 33.501 5.5.1/5.5.2, finding F2); the 3GPP documents defer to SAGE.
- Unverified (not available): S-box SR and SQ tables, the MULx/MULxPOW constants, LFSR/FSM update equations, initialization and clocking counts, the EEA1/EIA1 IV construction, MAC finalisation, every test vector.

## What unblocks this
Provide the SAGE Document 2 (SNOW 3G spec) and Document 3 (test data), obtained under the ETSI/GSMA
Restricted Usage Undertaking, and state whether the licence permits their algorithms/vectors to live in an
Apache-2.0 repository. Without that licence answer the vectors should stay out of the repo (tests could
read them from an untracked path). Do not accept an unlicensed third-party implementation as a substitute.
