## ADR-0198: UDR `Nudr_GroupIDmap` -- real generated DTOs, replacing hand-written ones

### Context

Found during ADR-0193's audit: `nfs/udr/src/main.cpp`'s own `CreateGroupIdSubscription` handler
(ADR-0170) hand-wrote `nlohmann::json` field-presence checks
(`!body.contains("notificationUri") || ...`) instead of using a real generated DTO, with a comment
admitting "no generated DTO for this API" -- because `TS29504_Nudr_GroupIDmap.yaml` had never been
added to `libs/sbi-generated/CMakeLists.txt`'s pilot set. A standing violation of this project's
own "never hand-write a DTO the YAML can generate" rule (CLAUDE.md), not a fabrication -- the
fields checked were the real required ones -- but real generated validation is strictly better
(catches type mismatches the hand-rolled `.contains()` check couldn't) and closes a real,
disclosed gap in this project's own codegen discipline.

### Implementation

`TS29504_Nudr_GroupIDmap.yaml` added to `libs/sbi-generated/CMakeLists.txt`'s pilot set (real
cross-file dependencies `TS29510_Nnrf_NFManagement.yaml`/`TS29571_CommonData.yaml` already wired).
Real generated types land in their own standalone header, `TS29504_Nudr_GroupIDmap.hpp` -- real
name collision found and resolved by the codegen itself: `SubscriptionData` is also declared by
other already-wired services, so the real generated type is
`sbi_gen::SubscriptionData_Nudr_GroupIDmap` (plus `RoutingIdResult`, `GroupIdMapNotify`, both
collision-free). `CreateGroupIdSubscription`'s handler now uses
`sbi_core::http2::parse_json_body<sbi_gen::SubscriptionData_Nudr_GroupIDmap>` in place of the
hand-rolled `.contains()` checks -- same real validation-by-generated-DTO discipline every other
UDR write route already uses. `GetRoutingIDs`/`GetNfGroupIDs`'s own GET responses were left as raw
`nlohmann::json` passthrough of already-seeded, schema-conformant data -- deliberately out of
scope for this fix, since they were never hand-written request-body validation in the first place
(same "ad-hoc response map" shape as several other real UDR/NRF resources in this project), so
widening this change to touch them would be scope creep beyond the actual flagged violation.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process, real `nrf` for OAuth2: `POST
/nf-group-ids/subscriptions` with a real, complete body (`notificationUri`/`nfType`/`nfGroupId`)
-> real `201` with the real generated fields, including the server-set `subscriptionId`; the same
request with `nfType` omitted -> real `400`, with the real generated struct's own required-field
access throwing `json.exception.out_of_range.403`, caught by the same `parse_json_body` error path
every other UDR/NF write route already uses -- a real, precise error message the old hand-rolled
`.contains()` check could only approximate.

### Testing

Full project rebuild clean. Full `ctest` (excluding the two known-flaky tests): 369/369 pass (no
new automated test needed -- this is an internal validation-mechanism swap behind an
already-tested real route, not new coverage).

### What this ADR does NOT include

`QueryGroupIdSubscription`/`ModifyGroupIdSubscription`/`RemoveGroupIdSubscription`'s own response
bodies stay as raw JSON passthrough of the stored document (the same document a real
`SubscriptionData_Nudr_GroupIDmap` would produce, since it originated from one at create time) --
not re-typed on every read/patch/delete, since there's no new validation value in doing so, only
churn. The real `onGroupIdMapChange` webhook callback's own disclosed non-implementation
(ADR-0170) is unaffected by this change.

