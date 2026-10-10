## ADR-0099: gap-closure task #106 continuation -- UDR real Message Waiting Data (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (13 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0098). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/context-data/mwd` (Message Waiting
Data (Document), real schema `MessageWaitingData` -- a `mwdList` of `SmscData` entries, each a
real SMSC address, either a `smscMapAddress` (`E164Number`) or a `smscDiameterAddress`
(`NetworkNodeDiameterAddress`, a real nested object with mandatory `name`/`realm`, not a plain
string -- confirmed the hard way during live verification below)) has a real, distinct
`PUT`/`GET`/`PATCH`/`DELETE` operation set: `CreateMessageWaitingData`, `QueryMessageWaitingData`,
`ModifyMessageWaitingData` (real `application/json-patch+json`, RFC 6902, same standard
`IpSmGwContextStore`'s own patch already uses), `DeleteMessageWaitingData`. Real, disclosed
difference from `IpSmGwContextStore`'s own PUT: MWD's real `PUT` genuinely distinguishes
`201 Created` from `204` (updated) per the YAML, confirmed by direct read, not assumed uniform
with `ip-sm-gw`'s own always-`204` PUT.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_mwd` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `MessageWaitingDataStore` class (`put`/`get`/
  `apply_patch`/`remove`) -- `put()` reuses `AmfContextStore`'s own real `xmax = 0` UPSERT idiom
  to genuinely distinguish insert-vs-update in one statement (not `IpSmGwContextStore`'s simpler
  always-update `put()`, since MWD's real spec response codes require the distinction).
- `nfs/udr/src/main.cpp`: four new routes (`PUT`/`GET`/`PATCH`/`DELETE`) at
  `/subscription-data/{ueId}/context-data/mwd`, reusing the already-vendored
  `sbi_gen::MessageWaitingData` DTO (confirmed present in the same generated group file as
  `IpSmGwRegistration` before writing any application code -- no new codegen work needed) and two
  new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PUT` with `{"mwdList":[{"smscMapAddress":"+15551234567"}]}` ->
real `201 Created` with `Location` header and the created document in the body (first-create path,
confirmed distinct from the update path below); `GET` immediately after -> real `200` with the
identical document. A first attempt at the real-update path (`PUT` again with a second
`smscDiameterAddress` entry given as a plain string) correctly returned a real `400`
(`ProblemDetails`, "Missing or invalid mandatory IE") -- not a bug: `NetworkNodeDiameterAddress`
is a real nested object requiring `name`+`realm`, confirmed by re-reading
`TS29503_Nudm_UECM.yaml` directly; retried with a spec-correct object and got real `204` (update
path, confirmed distinct from the `201` create path above). `PATCH` with a real RFC 6902
`replace` on `/mwdList/0/smscMapAddress` -> real `204`; `GET` again -> real `200` with the patched
value correctly reflected. `DELETE` -> real `204`; `GET` again -> real `404`. Direct `psql` query
against `udr_mwd` independently confirmed the persisted two-entry `mwdList` document matches what
the API returned.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) -- SMSF
itself, the real originator of MWD data, doesn't exist as a built NF in this project yet (Tier 2).
This closes UDR resource #14 of free5GC's ~42+; roughly 28 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

