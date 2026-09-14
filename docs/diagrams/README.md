# Diagrams

One directory for every diagram the project has or owes. Sources are Mermaid (`.mmd`): text,
diffable, rendered natively by GitHub in the viewer's theme. Conventions (solid = built and tested,
dashed = in scope and not built) are in `../ARCHITECTURE.md`.

## Present

| File | What it shows | Published in | Owner of truth |
|---|---|---|---|
| `architecture.mmd` | System architecture: NFs, charging, BSS, datastores, event bus, AI plane, observability, with built/planned distinguished | `README.md` (embedded verbatim) | this file; `tools/diagrams/check_readme_sync.py` fails if README's copy drifts |

## Pending -- recorded as tasks, 2026-09-14 (user-directed)

Each becomes a file here when it is drawn from what exists, never before. A diagram of something
that is not built is a roadmap slide, and would be labelled as one.

| # | Diagram | What it must be drawn from | Blocked on |
|---|---|---|---|
| D1 | **Deployment diagram** | `deploy/docker/docker-compose.yml` (32 services, 6 Postgres, Doris, Kafka, Valkey) and `deploy/helm/` (7 of 18 NFs) | nothing -- can be drawn now |
| D2 | **ERD -- charging & BSS** | `nfs/chf/schema.doris.sql`, `schema.postgres.sql`, `schema.features.doris.sql`, `bss/*/schema.sql` | nothing -- can be drawn now |
| D3 | **ERD -- subscriber & policy** | UDR, subscriber-management, PCF policy data | nothing |
| D4 | **Object diagram -- a charging session** | one real N40 Create/Update/Release with its Valkey refs, RatingDecision rows, CDR rows and Kafka events, taken from a live run | nothing |
| D5 | **Security -- SBI trust model** | TS 33.501 clause 13 as implemented: mTLS, NRF as OAuth2 AS, ES256 tokens, where SEPP/N32 would sit (dashed), and the four open non-conformances from ADR-0354 | nothing |
| D6 | **Security -- Lawful Interception architecture** | TS 33.127 clause 5/6/7.22: ADMF/LIPF, POIs per NF, CHF IRI-POI, NRF SIRF, MDF2/3, LI_X1/X2/X3/HI | draw as the design *before* building LI (blocker #0); all dashed until then |
| D7 | **Sequence -- 5G-AKA + NAS SMC + registration** | `test_amf_ngap_handover.cpp`'s real flow | nothing |
| D8 | **Sequence -- N28/Sy spending-limit change reaching SMF** | ADR-0328 | nothing |
| D9 | **Data-plane -- event bus and AI pipeline** | ADR-0350/0355: CHF -> Kafka -> Doris -> feature store -> training -> ONNX -> CHF | nothing; extend when NWDAF lands |
| D10 | **NWDAF ecosystem -- AnLF, MTLF, DCCF, ADRF, MFAF** and their stores (Valkey, Doris, Postgres+MLflow, Kafka) | ADR-0359's architecture; TS 23.288 5.1/5A/5B/5C; the 17 Nnwdaf/Ndccf/Nadrf/Nmfaf YAML files | draw now from ADR-0359 as design (dashed), solidify per function as each lands |

Adding a diagram type not listed here is expected -- this table is the queue, not the ceiling.
