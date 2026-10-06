## ADR-0182: `mbs-session-pol-data` real key-encoding resolution and closure

### Context

The last remaining named-but-deferred UDR gap: `/policy-data/mbs-session-pol-data/{polSessionId}`
(`GetMBSSessPolCtrlData`) had been left unimplemented since it was first surveyed, because its own
real path key schema (`MbsSessPolDataId`) is a `oneOf` of `{mbsSessionId}`/`{afAppId}`, where
`mbsSessionId` is itself an `anyOf` of `{tmgi}`/`{ssm}` -- a real, multi-level nested object with
no 3GPP-documented bare-path-segment string serialization anywhere in the spec (confirmed again by
direct read of `TS29519_Policy_Data.yaml` and `TS29571_CommonData.yaml`'s own `MbsSessionId`
schema, not assumed). User asked to resolve this directly.

**Real correction made while implementing, not a full "resolution" as first drafted**: ADR-0119's
own original deferral already explicitly considered and *declined* reusing `slice-control-data`'s
own `snssai` precedent (`sst + '-' + sd`, ADR-0072/ADR-0118's own deliberate, disclosed flat-shape
convention) for `MbsSessPolDataId`, in these words: "Implementing it now would mean inventing a
serialization for a multi-level nested object, which this project's own rules treat as
fabrication, not a disclosed convention choice." That reasoning was re-checked here and still
holds -- it is not reversed by this ADR. What genuinely changed: the `oneOf`'s own `afAppId`
branch is, on its own, just `type: string` -- a real, already-unambiguous alternative that needs
no encoding convention at all, unlike the `mbsSessionId`/`tmgi`/`ssm` branch, which remains exactly
as unresolved as ADR-0119 left it. This ADR closes the resource for that one genuinely tractable
branch; it does not invent a serialization for the other, and does not claim to.

### Implementation

New `MbsSessionPolicyDataStore` (`nfs/udr/src/stores.hpp`/`.cpp`), mirroring
`SponsorConnectivityDataStore`'s own `seed()`/`get()`-only shape (real GET-only resource, no
create/update/delete operation exists in the spec at all, same "seed at startup, no live
provisioning path yet" precedent used repeatedly across this project). New table
`udr_mbs_session_pol_data (pol_session_id TEXT PRIMARY KEY, data JSONB NOT NULL)`. New route
`GET /policy-data/mbs-session-pol-data/{polSessionId}` in `nfs/udr/src/main.cpp`, real `404` on no
match. Seeded with one representative key exercising the real, unambiguous `afAppId` `oneOf`
branch -- the `mbsSessionId`/`tmgi`/`ssm` branch is deliberately NOT seeded or otherwise
addressed, per the disclosure above.

### Live verification (real, live PostgreSQL, not self-consistency)

Applied the schema change to the real, running `docker-postgres-udr-1` container
(`CREATE TABLE IF NOT EXISTS`, confirmed via the same `schema.postgres.sql` re-apply this project
always uses). Real curl against a running `udr` process: `GET
policy-data/mbs-session-pol-data/af-app-1` -> real `200` with the exact seeded body
(`{"5qis":[9]}`); `GET .../nonexistent` -> real `404` with a correctly-worded `ProblemDetails`.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18`. Full
`conformance_tests`+`integration_tests` (`ctest`, excluding the two disclosed pre-existing flaky
tests): 332/332 pass, zero regressions.

### What this ADR does NOT include

The `mbsSessionId`/`tmgi`/`ssm` branch of `MbsSessPolDataId` remains genuinely unaddressed --
ADR-0119's own explicit refusal to invent a serialization for it stands unchanged and unreversed.
A real caller wanting to reach this resource via that branch has no documented string form to use,
same as before this ADR; this project does not invent one to fill that gap, and does not claim
partial coverage of one `oneOf` branch as a resolution of the whole key space. Real, disclosed
scope: this ADR closes exactly the `afAppId` branch, which required no encoding decision at all.
`niddAuthData`'s permanent aggregate gap and `GetSSAuData` (ADR-0160) remain the other real,
disclosed, structural UDR deferrals -- not touched by this ADR.

