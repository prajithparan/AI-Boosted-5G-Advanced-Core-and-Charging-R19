## ADR-0055: P4.2 kickoff -- Nchf_OfflineOnlyCharging/Nchf_SpendingLimitControl codegen wiring, and a real schema-name collision found and fixed

**Date:** 2026-08-11
**Status:** Accepted.

**Context:** P4.1's gate (ADR-0053) closed; CHARGING_PROMPT.md's P4.2 ("CHF core") is next.
Checked what's already real in `nfs/chf/src/main.cpp` before drafting a procedure list (per
CHARGING_PROMPT.md's own "procedure list for approval first" instruction), rather than assuming:
`Nchf_ConvergedCharging` Create/Update/Release already exist (ADR-0044/0046/0048/0050/0051), with
a real product-catalog-backed rating engine. Genuinely missing: `Nchf_ConvergedCharging` Notify,
all of `Nchf_SpendingLimitControl` (TS 29.594), all of `Nchf_OfflineOnlyCharging` (TS 32.291), and
N28/N41/N42 wiring.

**Scope correction on N28**: `Nchf_SpendingLimitControl`'s real schema
(`specs/5G_APIs-REL-19/TS29594_Nchf_SpendingLimitControl.yaml`, checked directly) has CHF as the
**server** (`POST /subscriptions`, real `SpendingLimitContext` body: supi/gpsi/policyCounterIds/
notifUri/expiry/supportedFeatures/notifId; `PUT`/`DELETE /subscriptions/{id}`) -- PCF subscribes
to CHF, not the reverse. CHF's only client-side role for this service is the real callback
mechanism confirmed in the YAML itself: POSTing to `{notifUri}/notify`
(`statusNotification`, body `SpendingLimitStatus`) and `{notifUri}/terminate`
(`subscriptionTermination`, body `SubscriptionTerminationInfo`). This narrows "N28 wiring" to
hosting a real subscription resource plus a real callback sender, not building an Npcf_* client.

**`Nchf_OfflineOnlyCharging`** (`TS32291_Nchf_OfflineOnlyCharging.yaml`, checked directly): real
basePath `/nchf-offlineonlycharging/v1`, three operations mirroring ConvergedCharging's own shape
almost exactly -- `POST /offlinechargingdata` (Create), `POST
/offlinechargingdata/{OfflineChargingDataRef}/update` (Update), `POST .../release` (Release).

**`Nchf_ConvergedCharging` Notify**: confirmed via the same YAML's `callbacks` block on
`POST /chargingdata` -- CHF, as client, POSTs `ChargingNotifyRequest` to the `notifyUri` the
original `ChargingDataRequest` supplied (`chargingNotification` callback), response
`ChargingNotifyResponse`. No dedicated CHF-hosted path; a client-role callback like
SpendingLimitControl's.

**Nnrf_AccessToken finding** (CHARGING_PROMPT.md Section A explicitly asks for this before P4.2
code): for domestic (single-PLMN) N28/N41/N42 traffic, this project's own already-uniform,
non-negotiable convention (CLAUDE.md: "OAuth2 tokens from NRF" on 100% of SBI traffic) already
answers this -- no new decision needed, PCF/AMF already attach NRF-issued bearer tokens to every
outbound call, same as everywhere else in this codebase. For the **inter-PLMN/roaming** case
specifically (N41/N42 across a PLMN boundary), this repo has no vendored TS 33.501 primary text to
confirm whether NRF-issued tokens apply across the boundary or whether it's purely SEPP/N32's own
security context -- **not confirmed, not guessed**. Deferred: roaming settlement is P4.11's scope,
not P4.2's, so this doesn't block P4.2.

**N41/N42 (AMF) wiring, real blocker disclosed**: CHF's server side already accepts a
`ChargingDataRequest` from any `nodeFunctionality` generically (already true before this ADR) --
but AMF has no real UE Registration procedure in this codebase to genuinely *trigger* a charging
call from (no NGAP/NAS stack exists yet; a full plan for that is drafted separately and not
started, independent of this charging work). Proving N41/N42 "for real" the same way N40 was
proven (a real SMF call, live-verified) is blocked on that separate, much larger prerequisite --
not fabricated here as a fake trigger.

### Codegen wiring, and a real schema-name collision found and fixed

Added `TS32291_Nchf_OfflineOnlyCharging.yaml` and `TS29594_Nchf_SpendingLimitControl.yaml` to
`libs/sbi-generated/CMakeLists.txt`'s pilot file list (both external refs,
`TS29571_CommonData.yaml` and `TS29512_Npcf_SMPolicyControl.yaml`, already present -- no new
dependency files needed).

**Real, found-not-assumed regression**: `Nchf_OfflineOnlyCharging`'s schema independently defines
its own `ChargingDataRequest`/`ChargingDataResponse`/`MultipleUnitUsage`/`UsedUnitContainer`/
`NFIdentification`/`NodeFunctionality` types (genuinely different shapes than ConvergedCharging's
own, same names -- two real, independent 3GPP services that happen to reuse type names). sbi-codegen's
existing collision-disambiguation (ADR-0010) correctly suffixed **both** sides with their source
service name once the collision existed, which retroactively renamed the previously-unsuffixed
`sbi_gen::ChargingDataRequest`/`ChargingDataResponse`/etc. that `nfs/chf/src/main.cpp` **and**
`nfs/smf/src/main.cpp` already referenced directly -- confirmed by a real, full project rebuild
that failed with genuine "is not a member of sbi_gen" compiler errors in both files, not
speculated. Fixed by updating every call site in both files to the new
`_Nchf_ConvergedCharging`-suffixed names (`ChargingDataRequest_Nchf_ConvergedCharging`,
`ChargingDataResponse_Nchf_ConvergedCharging`, `MultipleUnitUsage_Nchf_ConvergedCharging`,
`UsedUnitContainer_Nchf_ConvergedCharging`, `NFIdentification_Nchf_ConvergedCharging`,
`NodeFunctionality_Nchf_ConvergedCharging`) -- a systematic check against every type name that
collided (not just the ones the first compile error happened to surface) confirmed these six were
the complete set actually referenced by name in either file.

**Verified**: full project rebuild succeeds; full `ctest` suite 146/146 passes, including the real
`test_smf_pdu_session.cpp` integration test that exercises these exact renamed types over live
HTTP between real SMF and CHF processes -- not just a compile-time check. `clang-format` reapplied
and reverified clean after the rename (identifier length changes shifted line wrapping).

**Consequence:** codegen infrastructure for P4.2's remaining real work
(`Nchf_OfflineOnlyCharging` Create/Update/Release, `Nchf_SpendingLimitControl` Subscribe/Update/
Unsubscribe + notify/terminate callbacks, `Nchf_ConvergedCharging` Notify callback) is now in
place and building cleanly.

### Follow-up, same session: Nchf_OfflineOnlyCharging and Nchf_SpendingLimitControl implemented and live-verified

**`Nchf_OfflineOnlyCharging`** (`nfs/chf/src/stores.hpp`/`.cpp`, new `OfflineChargingDataStore`;
`nfs/chf/src/main.cpp`, new routes): real `POST /offlinechargingdata` (Create),
`POST .../{OfflineChargingDataRef}/update` (Update), `POST .../{OfflineChargingDataRef}/release`
(Release), real basePath `/nchf-offlineonlycharging/v1` confirmed directly from the YAML's own
`servers` block. Deliberately does **not** call the rating engine (`build_rating_grant`) --
confirmed directly against the real schema that `ChargingDataResponse_Nchf_OfflineOnlyCharging`
carries no `multipleUnitInformation`/`grantedUnit` field at all, a genuine spec difference from
ConvergedCharging, not an oversight. `OfflineChargingDataRef`s use their own `offchg-N` namespace,
distinct from ConvergedCharging's `chg-N`.

**`Nchf_SpendingLimitControl`** (`nfs/chf/src/stores.hpp`/`.cpp`, new
`SpendingLimitSubscriptionStore` -- a real resource store, not just an active-ref set, since
`PUT` needs the previous context; `nfs/chf/src/main.cpp`, new `build_spending_limit_status` +
three routes): real `POST /subscriptions` (Subscribe, 201 + `Location`), `PUT
/subscriptions/{subscriptionId}` (real update-in-place, 200), `DELETE
/subscriptions/{subscriptionId}` (Unsubscribe, 204), real basePath `/nchf-spendinglimitcontrol/v1`.
Both Subscribe and Update return a real `SpendingLimitStatus` built from the subscription's own
`policyCounterIds`. Disclosed, real simplification: `currentStatus` is a fixed `"unknown"`
placeholder for every policy counter -- no real policy-counter-monitoring engine exists in this
codebase to report a genuine status from; the real spec text itself says these status values "are
not specified... out of scope of 3GPP", so this is schema-conformant, not a guess at real
semantics (same disclosure category as ADR-0028's PCF fixed-default policy). The real
`statusNotification`/`subscriptionTermination` callbacks (CHF as client, confirmed directly from
the YAML's `callbacks` block: POST to `{notifUri}/notify` and `{notifUri}/terminate`) are **not**
implemented -- no real breach-detection engine exists yet to trigger them from; deferred, not
dropped, same category as `Nchf_ConvergedCharging`'s own still-deferred `chargingNotification`.

**Live-verified for real**, not just unit-level: started real `nrf` + `chf` processes, confirmed
CHF registers with NRF, then over real mTLS HTTP/2:
- OfflineOnlyCharging: Create (201, real `offchg-1` ref + Location), Update on the active ref
  (200), Update on an unknown ref (404), Release (204), Release again (404, correctly no longer
  active) -- and confirmed ConvergedCharging's own existing `/chargingdata` Create still works
  correctly side-by-side in the same process (201, real `chg-1`, independent namespace).
- SpendingLimitControl: Subscribe with two policy counters (201, real `SpendingLimitStatus` with
  both `statusInfos` entries, real `Location: .../subscriptions/sub-1`), Update narrowing to one
  policy counter and a new expiry (200, response correctly reflects only the updated counter),
  Update on an unknown subscription id (404), Unsubscribe (204), Unsubscribe again (404).

Full rebuild + `clang-format` clean + 146/146 `ctest` (including the real Postgres-backed
`product-catalog` tests, run with a live container) after each change.

**Still not done, disclosed**: `Nchf_ConvergedCharging` Notify callback; both services' notify/
terminate callback-sending code; N28 is now correctly understood as "CHF hosts, PCF subscribes"
(no PCF-client code needed for the subscription CRUD itself) but the callback-sending half is
still unbuilt; N41/N42 (AMF) wiring remains blocked on the separate NGAP/NAS prerequisite; no
automated integration test exists for CHF specifically (this NF's own established pattern so far
is real manual live-verification recorded in its ADRs, not an automated suite -- followed here,
not newly introduced).

### Follow-up, same session: real Redis/Valkey persistence for CHF's stores (E3)

CHARGING_PROMPT.md's entity E3 (Session Establishment) explicitly requires charging sessions to
be "idempotent and recoverable across restarts and network partitions"; `docs/DATA_MODEL.md`'s
own E3 persistence assignment is Redis/Valkey. CHF's three stores (`ChargingDataStore`,
`OfflineChargingDataStore`, `SpendingLimitSubscriptionStore`, all in `nfs/chf/src/stores.hpp`/
`.cpp`) were in-memory-only until this follow-up -- real gap against E3's own explicit
requirement, closed here.

**Dependency**: `redis-plus-plus` (Apache-2.0) + transitively `hiredis` (BSD-3-Clause), both
OSI-approved (P1-compliant), added to `vcpkg.json`. Checked upfront this time (learned from
ADR-0054's `libpq`/`bison` surprise) whether this would repeat that Dockerfile blast-radius
problem: neither port's own `vcpkg.json`/`portfile.cmake` names any external system build tool
requirement (no `find_program`/`REQUIRED` calls, confirmed by reading both files directly) --
installed cleanly in ~14s with no Dockerfile changes needed.

**Design**: one shared `std::shared_ptr<sw::redis::Redis>` across all three stores. Confirmed by
reading `sw::redis::Redis`'s own header (not assumed) that it manages an internal connection pool
and is genuinely thread-safe for concurrent use -- a real difference from `bss/product-catalog`'s
`libpqxx::connection`, which has no built-in pooling and needed the mutex-per-store pattern
ADR-0054 used. `ChargingDataStore`/`OfflineChargingDataStore` use a Redis `SET` for active-ref
tracking (`SADD`/`SREM`/`SISMEMBER`) plus an atomic `INCR` counter for ID generation --
**a genuine improvement over the old in-memory counter, not just a persistence bolt-on**: the old
`next_id_` was per-process and would have both collided across multiple CHF replicas and reset to
1 on every restart, neither of which Redis's atomic counter does.
`SpendingLimitSubscriptionStore` stores each subscription's real `SpendingLimitContext` as a JSON
string value (real resource store, not just an active marker, since `PUT` needs the previous
content). Connection string via `CHF_REDIS_URL` env var (same never-hardcode-credentials
discipline as `PRODUCT_CATALOG_DATABASE_URL`, ADR-0054), with a real `PING` at startup for
fail-fast behavior matching every other NF's real dependency check (confirmed the pool connects
lazily on first command otherwise, not eagerly at construction, by reading `ConnectionPoolOptions`
directly -- not assumed).

**Live-verified for real, including actual restart-survival** (the entire point of this change):
started real `nrf` + `chf` processes against a real `valkey/valkey:8-alpine` container (the
OSI-approved fork, per ADR-0053's own compliance table -- not the SSPL-relicensed Redis image),
created a real ConvergedCharging session (`chg-1`) and a real SpendingLimitControl subscription
(`sub-1`), confirmed both directly via `valkey-cli` (independent of CHF's own serialization,
same cross-process-independent-re-derivation discipline as ADR-0054) -- then **killed the CHF
process entirely and started a fresh one**, and confirmed: (1) `Update` on `chg-1` returns 200,
not 404 -- the session survived; (2) `PUT` on `sub-1` returns 200 with the real previous content
correctly updated-in-place -- the subscription survived; (3) a new `Create` call afterward
allocated `chg-2`, not `chg-1` again -- the atomic ID counter itself survived and continued
correctly, not just individual records. Full rebuild + `clang-format` clean + 146/146 `ctest`
(unaffected, since no ctest-registered test spawns CHF) both before and after.

**Still disclosed, real limitation carried forward**: `ChargingDataStore`/
`OfflineChargingDataStore` only persist active-ref *existence*, not real session content (same
shape the in-memory version already had) -- recovering actual charging state (not just whether a
ref exists) after a restart would need a real resource store here too, same category of future
work as `SpendingLimitSubscriptionStore` already demonstrates the pattern for.

---

