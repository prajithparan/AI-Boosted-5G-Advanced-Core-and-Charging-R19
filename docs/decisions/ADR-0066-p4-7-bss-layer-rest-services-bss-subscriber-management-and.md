## ADR-0066: P4.7 -- BSS layer REST services, `bss/subscriber-management` and `bss/roaming-interconnect`

**Date:** 2026-08-15
**Status:** Accepted. Stage 1 of the P4.7-P4.12 staged plan (agreed with the user, see the plan
saved this session) toward completing Phase 4 before Phase 5. Closes the disclosed HTTP-service
gap both `bss/subscriber-management/schema.sql` and `bss/roaming-interconnect/schema.sql` named
since ADR-0060 ("store library only this turn, no HTTP/REST service yet").

### What was built

Two new standalone HTTP/2 services, following `bss/product-catalog/src/main.cpp`'s own established
pattern exactly (sbi_core server, mTLS-only security boundary, real PostgreSQL via the existing
store classes, real Create/Get/List with PATCH/DELETE deferred -- same disclosed CRUD bar
product-catalog itself uses):

- **`bss/subscriber-management`** (port 7787): real **TMF632** `Individual`/`Organization` over
  `/tmf-api/party/v4/` (basePath confirmed directly from TM Forum's own public swagger,
  `github.com/tmforum-apis/TMF632_PartyManagement`, fetched live -- not recalled from memory), plus
  project-internal `Account`/`Subscriber` (E10/E1) over a clearly-non-TMF `/bss-api/
  subscriberManagement/v1/` basePath, including a `GET .../subscriber/by-supi/{supi}` convenience
  route (the store already had `get_by_supi`, previously with no HTTP path to reach it).
- **`bss/roaming-interconnect`** (port 7788): real **TMF651** `Agreement` over
  `/tmf-api/agreementManagement/v4/` (basePath confirmed the same way,
  `github.com/tmforum-apis/TMF651_AgreementManagement` -- real repo name differs from the naive
  guess `TMF651_Agreement`, found via a real GitHub search, not assumed).

Both stores' `list()` methods (previously absent -- only `create`/`get` existed) were added this
turn, matching `bss/product-catalog`'s own store shape, with a real regression: `pqxx::result::
size()` returns a signed `int`; `-Wsign-conversion` (this project's own enabled warning) caught the
narrowing on `reserve()` immediately, fixed with the same `static_cast<std::size_t>` product-catalog
's own `list()` methods already used -- not a new pattern.

### Real, disclosed scope refinement: `InterconnectAgreement` moved from P4.11 to P4.7

`bss/roaming-interconnect/schema.sql`'s original text (ADR-0060) assigned E7's entire HTTP service
-- `InterconnectAgreement` AND `RoamingCdrFile` together -- to P4.11, since P4.11 (real roaming
settlement) is genuinely blocked on GSMA TAP3/RAP/NRTRDE spec text CLAUDE.md forbids fabricating.
On review this turn: `InterconnectAgreement` (who the partner operator is, real TMF651 Agreement
master data) is not GSMA-blocked at all -- it's ordinary master data, squarely part of P4.7's own
"master model" layer (`docs/DATA_MODEL.md`'s E10 framing already treats Agreement as part of the
enterprise/SLA hierarchy). `RoamingCdrFileStore` (the actually GSMA-blocked piece, real CDR
ingestion) still has no HTTP route -- real, disclosed, still P4.11's own scope, not silently
narrowed away. Both `schema.sql` files' own headers updated to record this split rather than left
stale.

### Real bug found and fixed: `store.hpp` filename collision across three `bss/*` libraries

`tests/integration/CMakeLists.txt` now links `integration_tests` against three separate `bss/*`
store libraries (`product_catalog_store`, `subscriber_management_store`,
`roaming_interconnect_store`), each with `target_include_directories(... PUBLIC src)` and each
naming its own header `src/store.hpp` -- identical filenames in three different directories. A bare
`#include "store.hpp"` in the two new test files silently resolved to
**`bss/product-catalog/src/store.hpp`** instead of the correct one (whichever `-I` directory CMake
happened to list first, determined by `target_link_libraries` order) -- confirmed via the real
compiler error (`'subscriber_management' has not been declared`, with the actual resolved
`#include` path visible in the diagnostic trace pointing at `product-catalog/src/store.hpp`). Fixed
with explicit relative-path includes (`#include "../../bss/subscriber-management/src/store.hpp"`)
in both new test files -- a real, disclosed fragility in this repo's own build setup, not
pre-existing (product-catalog was the only `*_store` library before this turn, so no collision was
possible until a second and third one existed).

### Testing and live verification

Two new real-PostgreSQL integration tests (`tests/integration/test_subscriber_management_postgres
.cpp`, `test_roaming_interconnect_postgres.cpp`), same not-mocked/GTEST_SKIP()-if-unreachable
discipline as `test_product_catalog_postgres.cpp`. One real schema fact found while writing these:
`account.organization_id` is a real foreign-key constraint into `party_organization` -- an
arbitrary placeholder id fails at insert time; the test now creates a real `Organization` first.
Verified against real, freshly-created (not reused/stale) PostgreSQL containers: 6/6 new tests
pass.

Live-verified both services over real mTLS HTTP/2 with `curl` against real running binaries (real
Postgres containers, real lab PKI): `POST`/`GET` (list + by-id) for Individual, Organization,
Account, Subscriber (including the by-SUPI lookup), and InterconnectAgreement all round-trip
correctly; a `GET` for a nonexistent Individual returns a real `ProblemDetails` 404. `docker
compose up --build` for both new services (with their own dedicated `postgres-subscriber`/
`postgres-roaming` instances, same "one postgres per consumer" shape as balance-management/chf)
also live-verified, not just assumed from the compose file's own correctness.

CI (`.github/workflows/ci.yml`) extended with two new `postgres-subscriber`/`postgres-roaming`
service containers (both `build` and `sanitize` jobs), schema-apply steps, and the two new
`TEST_*_POSTGRES_URL` env vars -- CI now exercises the real DB path for both new stores, not
`GTEST_SKIP()`.

### Real, disclosed gaps NOT closed by this ADR

- PATCH/DELETE not implemented for any of the five new resource types, same disclosed narrowing
  `bss/product-catalog` already used.
- Neither service registers with NRF or verifies OAuth2 tokens -- same "not a 3GPP NF, no
  NRF-issued token source" reasoning every other `bss/*` component already carries.
- Subscriber's real trigger (`nfs/udm`/`nfs/udr` actually calling this service from a real SUPI
  lookup) does not exist -- this is the data model + API only, not yet wired into the
  control-plane NFs.
- Helm charts were not added for either new service -- consistent with this project's own actual
  established deploy-verification bar so far (Docker + compose, live-verified; Helm exists for 7
  NFs but has never itself been `helm install`-verified for anything in this repo, a separate,
  already-flagged cross-cutting gap, not newly introduced here).
- `docs/TRACEABILITY.md` gets a new entry for this ADR's own work (see below) but the file remains
  stale for ADR-0053 through ADR-0065 -- a full backfill is real, separate, disclosed, deferred
  work, not attempted in this pass.

Full rebuild + `ctest`: all pre-existing tests plus 6 new PostgreSQL-backed integration tests pass,
zero regressions.

