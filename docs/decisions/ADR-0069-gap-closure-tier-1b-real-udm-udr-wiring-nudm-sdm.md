## ADR-0069: gap-closure Tier 1b -- real UDM->UDR wiring (Nudm_SDM provisioned-data)

### Context

Tier 1a made UDR's `context-data` group genuinely persistent. This turn closes the second real
gap the free5GC/open5gs comparison found: `nfs/udm/src/main.cpp`'s `GetAmData`/`GetSmfSelData`/
`GetSmData` handlers always returned a schema-valid but empty/default response for ANY supi --
open5gs's own UDM has a real `Nudr_DataRepository` client (`src/udm/nudr-build.c`/
`nudr-handler.c`) that genuinely fetches subscriber data from UDR.

**Real, load-bearing discovery made while scoping this turn**: the resource UDM's SDM operations
need is NOT UDR's `context-data` group (AmfContext3gpp/SmfRegistration, Tier 1a's own scope) --
it's a different real Nudr_DataRepository resource group, `provisioned-data`
(TS29505_Subscription_Data.yaml's `/subscription-data/{ueId}/{servingPlmnId}/provisioned-data/
am-data|smf-selection-subscription-data|sm-data`), which UDR's own file header had explicitly
deferred ("GET-only in this spec, no way to provision it through this API at all, so implementing
it now would just be another permanently-empty stub"). Closing this gap therefore required
extending UDR with a real new resource group AND wiring UDM to call it -- one cohesive
gap-closure subsystem turn (matching this session's own established granularity for tightly
coupled client/server pairs), not two separate NF turns.

### What changed

**UDR** (`nfs/udr/`): new `udr_provisioned_data` table (`schema.postgres.sql`), keyed by
`(ue_id, serving_plmn_id)` per the real path shape, three nullable `JSONB` columns (`am_data`,
`smf_sel_data`, `sm_data`). New `ProvisionedDataStore` class (`stores.{hpp,cpp}`) -- real, disclosed:
NO `put()`/`apply_patch()` exists, only `seed()` (idempotent UPSERT) and three real `get_*()`
accessors, since this resource group is confirmed GET-only in the real YAML (no create/update
operation defined at all -- the real provisioning path in a production deployment is an
out-of-band OSS/BSS tool writing directly into UDR's backing store, not this public API). Three
new real GET routes in `main.cpp`. Seeded at startup for the same two real test SUPIs
(`imsi-999700000000001`/`...002`, UERANSIM's own real values) `nfs/udm`'s own
`AuthenticationSubscriptionStore` already seeds, so a real end-to-end chain has real data for at
least these subscribers. Seed content: `AccessAndMobilitySubscriptionData.nssai` and
`SessionManagementSubscriptionData.singleNssai` populated with this project's own real lab
S-NSSAI (sst=1, sd="000001", ADR-0016); `SmfSelectionSubscriptionData.subscribedSnssaiInfos` and
`SessionManagementSubscriptionData.dnnConfigurations` left real, disclosed, unpopulated -- both
are `OPAQUE FALLBACK` (codegen couldn't strongly type them), and this session's own time-boxed
spec reading could not confirm their real nested key/value shape with confidence -- left empty
rather than guessed, per CLAUDE.md's fabrication rule.

**UDM** (`nfs/udm/`): new `udr_client`/`udr_oauth` (same separate-`http2::Client`-per-target-NF
pattern `nfs/ausf/src/main.cpp`'s own `udm_client` already established). New
`resolve_serving_plmn_id()` helper -- real handling of the genuinely OPTIONAL `plmn-id` query
parameter (TS29503_Nudm_SDM.yaml, checked not assumed), parsing a real `PlmnIdNid` JSON value if
present, falling back to this project's own real lab PLMN (`"99970"`, mcc=999/mnc=70 concatenated
per the real `VarPlmnId` string format) if absent or malformed. `GetAmData`/`GetSmfSelData`/
`GetSmData` now issue a real `Nudr_DataRepository` GET via a shared `fetch_from_udr` helper,
returning UDR's real data on 200, a real 404 `ProblemDetails` if UDR has no data for that SUPI
(instead of the old stub's always-200-empty), and a real 500 if UDR is unreachable or returns
malformed JSON.

`deploy/docker/docker-compose.yml`: `udm` now `depends_on: udr` (`service_started`), same pattern
`ausf`'s own dependency on `udm` already established.

### Real end-to-end verification (not just unit-level)

Started real `nrf`+`udr`+`udm` processes against a real, freshly-created local PostgreSQL
container; `curl`'d `GET /nudm-sdm/v2/imsi-999700000000001/am-data` through real mTLS HTTP/2 --
returned the real seeded `nssai` data (sst=1/sd="000001"), not an empty body. Same for
`smf-select-data`/`sm-data`. A genuinely unseeded SUPI (`imsi-999999999999999`) correctly returned
a real `404 Not Found` `ProblemDetails`, proving the real UDM->UDR round trip, not a cached or
short-circuited response.

`tests/integration/test_udm_uecm_sdm.cpp`'s `SdmDataRetrievalAndSubscriptions` test updated (not
left stale): now spawns a real `udr` process alongside `nrf`/`udm` (this test's real new
dependency), asserts the real non-empty `nssai` content instead of just a 2xx status, and adds a
real 404-for-unseeded-SUPI case. `clang-format-18 --dry-run --Werror` clean. Full rebuild + `ctest`
(with a real local Postgres): 275/275 pass, zero regressions.

### What this ADR does NOT include

Live provisioning of `provisioned-data` from any real OSS/BSS system -- the spec's own GET-only
shape means this remains seed-data-only, same real, disclosed limitation as `nfs/udm`'s own
`AuthenticationSubscriptionStore`. `subscribedSnssaiInfos`/`dnnConfigurations`'s real nested shape
(flagged above, not resolved). UDM's own UECM registration groups
(`AmfRegistrationStore`/`SmfRegistrationStore`) remain NOT wired to UDR -- a real, separate,
still-open gap, out of this turn's scope (this turn closed SDM's provisioned-data gap only, the
one the free5GC/open5gs comparison actually found).

