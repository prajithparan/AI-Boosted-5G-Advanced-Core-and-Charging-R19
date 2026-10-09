# SNOW 3G / 128-EEA1 / 128-EIA1 -- source audit (Phase 0, 2026-10-09)

**Result: BLOCKED. No code has been written.** The normative algorithm text and test vectors are
not in any material available to this project.

| Reference | Version | What it contains | Technical content for implementation? |
|---|---|---|---|
| 3GPP TS 35.215 (UEA2/UIA2 Document 1) | Rel-19 j00, fetched with `tools/specs/fetch_3gpp_specs.py` | ZIP with one .docx, 195 lines: scope/foreword only | **No.** "The technical provisions ... are contained in the SAGE Specification" |
| 3GPP TS 35.216 (SNOW 3G, Document 2) | Rel-19 j00 | same cover-page structure | **No.** Same sentence |
| 3GPP TS 35.217 (implementors' test data) | Rel-19 j00 | same | **No** vectors in the file |
| 3GPP TS 35.218 (UEA2/UIA2 Document 4: design conformance test data) | Rel-19 j00 | same | **No** |
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

## Release 16 re-check (2026-10-09, ETSI-published PDFs, fetched with the WebFetch tool; plain curl got HTTP 403 and no workaround was attempted)

All four are ETSI TS 135 21x V16.0.0 (2020-08), Release 16, URL pattern
`https://www.etsi.org/deliver/etsi_ts/135200_135299/1352NN/16.00.00_60/ts_1352NNv160000p.pdf`,
freely downloadable, each **8 pages**, ~1,415-1,426 words each, **zero hex constants**. Pages: 1 cover,
2-3 copyright and IPR notices, 4 contents, 5 foreword + scope + references + definitions + "Technical
provisions" (one sentence), 6-7 history, 8 trailer.

| TS | Title (Document n) | Technical provisions |
|---|---|---|
| 35.215 | Document 1: UEA2 and UIA2 specifications | "contained in the SAGE Specification [2]" |
| 35.216 | Document 2: SNOW 3G specification | same |
| 35.217 | Document 3: Implementors' test data | same |
| 35.218 | Document 4: Design conformance test data | same |

Usage terms printed in each: "No part may be reproduced or utilized in any form or by any means ... except
as authorized by written permission of ETSI" (copyright notice, (c) ETSI 2020). The referenced SAGE
specification v1.1 (2006-09-06) is "subject to licensing conditions" at the ETSI algorithms page
(Restricted Usage Undertaking + administrative fee).

### Per requested item
- Core algorithm, initialization, state transitions, substitution (S-box) transformations, keystream
  generation, F8/F9 interfaces: **none specified** in any of the four (same one-line pointer).
- Conclusion: **insufficient. Plan item 9 does not apply; this is the item 10 blocker report.**

### Missing information (precise list)
1. S-boxes SR and SQ (256-entry tables), the GF(2^8) reduction polynomials, and the MULalpha/DIValpha definitions.
2. LFSR length, feedback polynomial in GF(2^32), initialization mode vs keystream mode clocking.
3. FSM registers R1-R3, update equations, how the key and IV are loaded (word order) and the number of initialization rounds.
4. Keystream output rule and the first-output-discard.
5. UEA2 (128-EEA1) f8 keystream/IV construction and UIA2 (128-EIA1) f9 MAC construction (including FRESH/DIRECTION placement, the final-block handling and MAC extraction).
6. Known-answer vectors (SAGE Document 3 and 4).
Items 1-5 are in SAGE Documents 1-2; 6 in Documents 3-4. None is in TS 33.401/33.501, which delegate to them.

### Open question for the architect
Obtain the SAGE package under the ETSI/GSMA undertaking (and confirm the undertaking's terms permit the
algorithm to be implemented in, and tests to be committed to, an Apache-2.0 repository), then re-run this
audit. Until then no snow3g code is written and no conformance is claimed.
