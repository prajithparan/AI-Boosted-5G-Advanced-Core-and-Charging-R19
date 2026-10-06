## ADR-0079: gap-closure task #102 -- NRF real NFProfile validation + heartbeat-expiry timer

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s NRF section named two real gaps: (1) `NFProfile` semantic
validation was entirely absent -- `RegisterNFInstance` only checked that `nfInstanceId`/`nfType`/
`nfStatus` KEYS were present, never that their VALUES were well-formed, so a malformed `nfType` or
an out-of-range `heartBeatTimer` was silently accepted; (2) no active heartbeat-expiry timer --
a crashed NF that stopped sending `PATCH` heartbeats stayed registered forever, unlike open5GS's
real `t_no_heartbeat` mechanism. Both closed this pass.

### Real NFProfile validation, every constraint spec-grounded, not invented

Read the real OpenAPI YAML directly for every field checked (cited per-field in
`nfs/nrf/src/main.cpp`'s own comment), rather than assuming free5GC's ~290-line validator's
choices were themselves the source of truth:
- `nfInstanceId`: `format: uuid`, "shall be a Universally Unique Identifier (UUID) version 4"
  (`TS29571_CommonData.yaml`).
- `heartBeatTimer`: `type: integer, minimum: 1` (`TS29510_Nnrf_NFManagement.yaml`) -- no maximum
  is declared in the spec, so none is enforced.
- `nfType`: `TS29571_CommonData.yaml`'s `NFType` is a real "open" `anyOf[enum,string]` type (any
  string round-trips on the wire, confirmed via `sbi_gen::NFType`'s own generated comment) -- but
  NRF, as the registry owning the canonical NF catalog, should still reject unrecognized values at
  registration time. Validated against the exact real enum list the spec (and this project's own
  codegen) already derived, not independently re-invented.
- `nfStatus` / `nfServices[].nfServiceStatus`: real 4-value enum (`REGISTERED`/`SUSPENDED`/
  `UNDISCOVERABLE`/`CANARY_RELEASE`), both from `TS29510_Nnrf_NFManagement.yaml`.
- `nfServices[].scheme`: real `{http, https}` `UriScheme` enum (`TS29571_CommonData.yaml`).
- `nfServices[].ipEndPoints[].transport`: **TCP only** -- a real, deliberate finding, not
  free5GC's own arbitrary choice: this API's own LOCAL `TransportProtocol` schema
  (`TS29510_Nnrf_NFManagement.yaml`) is narrower than the general `TS29571_CommonData`
  `TransportProtocol` (which also allows UDP) -- confirmed by reading both definitions directly,
  not assumed from the more general type's name alone.
- `nfServices[].ipEndPoints[].port`: `minimum: 0, maximum: 65535` (`TS29510_Nnrf_NFManagement.yaml`).
- `ipv4Addresses[]` / `ipEndPoints[].ipv4Address`: real dotted-decimal regex, copied verbatim from
  `TS29571_CommonData.yaml`'s `Ipv4Addr` pattern.
- `ipv6Addresses[]` / `ipEndPoints[].ipv6Address`: real colon-hex regex pair, copied verbatim from
  `TS29571_CommonData.yaml`'s `Ipv6Addr` pattern (two `allOf` patterns both must match).

Implemented as plain JSON-field checks (`validate_nf_profile`, `nfs/nrf/src/main.cpp`) rather than
by parsing into the generated `sbi_gen::NFProfile_Nnrf_NFManagement` DTO -- the DTO's own open-enum
fields (`NFType`/`NFStatus`/etc.) accept any string without throwing by design (correct for wire
round-tripping), so parsing alone would not have caught any of these violations; real, semantic,
registry-level validation needed to be separate from wire-format parsing.

### Real heartbeat-expiry sweep

`NfRegistry` gained `touch_heartbeat`/`sweep_expired` (`nfs/nrf/src/registry.hpp/.cpp`): a
`last_heartbeat_` map updated on both initial registration and every later `PATCH`, and a periodic
background sweep (new `std::thread` in `main()`, 5s interval) that removes and fires
`NF_DEREGISTERED` for any NF whose OWN profile declared a `heartBeatTimer` and whose last
heartbeat exceeds `heartBeatTimer + margin`. Modeled on open5GS's real `t_no_heartbeat` mechanism
(`src/nrf/nf-sm.c`) but NOT a byte-for-byte port -- this project's own periodic-sweep design
rather than a per-NF timer object, and the interval/margin (5s/5s) are this lab's own disclosed
choice, not claimed to match open5GS's own specific (unpublished) numeric values. An NF that never
supplies `heartBeatTimer` is never swept -- nothing in the spec to expire it against, not
invented.

### Verification -- live, not just unit-level

Real, live HTTP verification against a running NRF (not simulated): a valid profile registers
(201); an unrecognized `nfType`, `heartBeatTimer=0`, a non-UUID `nfInstanceId`, and a malformed
IPv4 address are each independently rejected with a real, specific 400 `ProblemDetails` message.
The heartbeat-expiry sweep verified end-to-end: an NF registered with `heartBeatTimer=2` (margin
5s) was confirmed present via `GET`, then confirmed gone (404) ~13s later, with the real log line
"missed its heartBeatTimer -- deregistering"; a second NF registered with `heartBeatTimer=6`, sent
one `PATCH` heartbeat at t=4s, and was confirmed STILL present at t=12s (which would have expired
an unrefreshed timer) -- proving the heartbeat genuinely resets the expiry window, not just that
the sweep exists.

Full `conformance_tests` (261/261) unaffected (no new unit tests added this pass -- coverage came
from the live HTTP verification above instead). A full `ctest -j4` run surfaced 4 failures
(`SmfIntegration.FullSmContextLifecycleOverRealHttp2`,
`SmfIntegration.CreateSMContextFailsClosedWhenPcfUnreachable`,
`PcfN28Integration.CreateSmPolicyFailsOpenWhenChfUnreachable`,
`PcfChfN28Integration.FullLoopSubscribeStatusChangeNotifyUnsubscribe`); all 4 re-ran and passed
cleanly under `-j1` in isolation, confirming a real, PRE-EXISTING test-isolation gap (multiple
`ctest -j4` jobs spawning their own `nrf`/`pcf`/`chf` instances on the same fixed ports, 7777/7783/
etc., can collide and see each other's processes) rather than a regression from this ADR's own
changes -- not fixed here (a test-harness concern, not a product one), disclosed rather than
silently worked around.

### What this ADR does NOT include

`SearchNFInstances`'s `subscrCond` filtering gap (already self-disclosed in
`nfs/nrf/src/main.cpp` before this ADR, independently corroborated by the gap-analysis sweep, not
addressed here). NRF-to-NRF federation, full `searchOptions` completeness, and rate-limiting/TPS
protection remain out of scope, per `docs/CAPABILITY_GAP_ANALYSIS.md`'s own "not yet checked"
list.

