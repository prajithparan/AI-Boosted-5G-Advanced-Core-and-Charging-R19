## ADR-0464: CC chain -- CC-TF in the SMF, CC-POI in the UPF, X3, MDF3 (ADR-0463 stage 3)

**Date:** 2026-10-06. **Status:** stage 3 built; stage 4 (real packet capture) not started and `cc_capable` stays
false for the UPF in any real deployment until it is verified on a privileged lab host.
**What was built.** (1) CC-TF in the SMF (`smf::CcTfConfig`, `libs/li-poi/x1_trigger`): just before the N4 PFCP
Session Establishment Request leaves for a target's session the SMF sends an X1 ActivateTask (LI_T3) to the UPF's X1
server naming the PFCP session by F-SEID; it withdraws the task on release or on a failed establishment. (2) CC-POI in
the UPF (`nfs/upf/src/li_poi.cpp`) on `li_poi::PoiRuntime`; only the F-SEID target kind is matched, every other UPFLIT3
identifier is refused with X1 3010. (3) X3: one TS 103 221-2 PDU per packet, payload format 12 (GTP-U), FromTarget for
uplink, ToTarget for downlink. (4) `nfs/li-mdf3`: X1 from the ADMF, X3 in, HI3 CC out to the LEMF; warrant state in
Valkey under its own prefix (`libs/li-mdf-store`, extracted from li-mdf). (5) ADMF: `cc_capable` per element and an
`mdf3` role; a missing `target_elements` defaults to empty (the AMF set), not a startup crash.
**Proof.** `LiCcChain.*` (uplink + downlink packets of a target session reach the LEMF as HI3 CC; unsupported target
kinds refused), `LiSmfEndToEnd.TheCcTfTriggersTheUpfPoiForACcWarrantAndWithdrawsItOnRelease`, `LiAdmfEndToEnd` green
after the `target_elements` fix (found because the test's overlay omits the key and li-admf aborted after "starting").
**Disclosed gaps.** The UPF's `on_packet()` is a seam fed by tests; nothing in the datapath calls it (eBPF is uplink
decapsulation only and cannot attach in CI). The LI_MDF packet-header-report approach (6.2.3.9.1 approach 2) is not
mediated. `N28SyEndToEnd.SpendingLimitStatusChangeReachesTheSmfThatOwnsTheSession` fails locally (404 on the PCF
status push, `smpolicy-N` counter assumption) -- not yet diagnosed as pre-existing vs caused by the SMF changes.
