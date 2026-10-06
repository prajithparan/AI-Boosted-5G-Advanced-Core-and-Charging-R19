## ADR-0086: gap-closure task #107 (part 2, first slice) -- UPF real PFCP PFD Management

### Context

ADR-0084 closed 2 of the 5 real PFCP message types `docs/CAPABILITY_GAP_ANALYSIS.md` found missing
from this project's UPF (`AssociationUpdate`/`AssociationRelease`), deliberately deferring
`PFDManagement`/`NodeReport`/`SessionSetDeletion` since each needs a genuinely new IE type. This
ADR closes `PFDManagement` (TS 29.244 §7.4.3) -- real values `PfdManagementRequest=3`/
`PfdManagementResponse=4` confirmed against Table 7.3-1, the same table every other
`pfcp_core::MessageType` value is confirmed against.

### Real, spec-cited scope

Real IE tables read from `specs/PFCP/29244-e30.pdf` §7.4.3.1/§7.4.3.2 and §8.2.6/§8.2.39:
- **PFD Management Request**: 0+ "Application ID's PFDs" grouped IEs (type 58), each containing a
  mandatory Application ID (type 24, §8.2.6, a bare OctetString) and 0+ "PFD" grouped IEs (type 59
  -- real, disclosed naming inconsistency: called "PFD context" in the master IE table, Table
  8.1.2-1, but "PFD" in its own sub-table heading, Table 7.4.3.1-3; both names refer to the same
  real IE type 59), each containing 1+ PFD Contents IEs (type 61, §8.2.39: a flag octet selecting
  which of Flow Description/URL/Domain Name/Custom PFD Content sub-fields follow, each as its own
  2-byte-length-prefixed OctetString in that fixed order).
- **PFD Management Response**: Cause (M) + Offending IE (C, rejection-only).
- **Real, literal condition-text semantics** (Table 7.4.3.1-1/7.4.3.1-2's own "shall delete..."
  language, not inferred): if the top-level Application ID's PFDs IE is absent from the whole
  message, the UP function deletes every PFD stored for every Application ID; for each Application
  ID's PFDs group present, if its own PFD child is absent, the UP function deletes every PFD stored
  for just that Application ID; otherwise the PFDs carried in the message become that Application
  ID's complete new set (replace, not merge/append).

New `libs/pfcp-core/include/pfcp_core/pfd_ies.hpp`/`src/pfd_ies.cpp`: `encode`/`decode_application_id`
(thin wrapper, no real structure to parse) and `PfdContents` struct +
`encode`/`decode_pfd_contents` (the one real structured IE this pair needs). The two grouped IEs
(`ApplicationIdsPfds`, `PfdContext`) get no dedicated codec -- decoded via the existing generic
`decode_ies`/`find_ie` the same way `CreatePdr`/`CreateFar` already are (`session_ies.hpp`'s own
precedent). New `find_all_ies` helper in `nfs/upf/src/main.cpp` -- this project's first real need
for a multi-match IE lookup (`find_ie` only returns the first), since both grouped IEs in this
message are real, spec-permitted repeated groups.

New `PfdStore` in `nfs/upf/src/main.cpp` (mutex-guarded `unordered_map<application_id,
vector<PfdContents>>`, same shape as the file's own `SeidToTeidStore`), and
`build_pfd_management_response_ies` applying the literal replace/delete-one/delete-all semantics
above, wired into `run_pfcp_lifecycle`'s existing dispatch chain in the same position/style as
every other PFCP message type there.

### Real, disclosed scope narrowing

- **No Application Detection Filter (ADF) engine consumes this store.** This project's UPF has no
  traffic-classification data-plane logic that reads `PfdStore` at all -- PFDs can be received,
  replaced, and deleted correctly (this ADR's own real scope), but nothing downstream acts on them
  yet. A real, separate, much larger gap (`docs/CAPABILITY_GAP_ANALYSIS.md`'s own UPF section
  already named this), not fabricated here as a side effect of message-level correctness.
- **Response is unconditionally `Cause=RequestAccepted`, no Offending IE support.** Same disclosed
  precedent as ADR-0084's own AssociationUpdate/Release: a malformed inner group is logged and
  skipped rather than rejecting the whole message, since this project has no other real admission-
  control reason PFD provisioning would genuinely fail in this lab.
- **No automatic SMF-side trigger exists yet** -- SMF has never sent PFD Management in this
  project's existing call flows (real-world PFD Management is typically NEF/PFDF-driven per TS
  23.503, a different, not-yet-built control-plane path entirely). Live verification below used a
  hand-crafted client for the same reason ADR-0084's own verification did.

### Verification

Built clean (`cmake --build . --target pfcp_core upf conformance_tests -j4`), zero warnings from
the new code. 7 new unit tests (`tests/conformance/test_pfcp_core.cpp`, `PfcpPfdIes.*`): Application
ID round-trip, PFD Contents round-trip with all four/only-one field(s) present, empty-PFD-Contents
byte-length check, two malformed-input rejection cases, and a grouped-IE round-trip through the
existing generic codec (mirroring `PfcpSessionIes.GroupedIeRoundTripsViaExistingIeCodec`'s own
precedent) -- all pass.

Live, real raw UDP verification (`pfcp_assoc_verify.py`'s own sibling script, same scratch-tooling
class, not committed) against a standalone UPF instance, exercising all three real semantic paths:
(1) provision 2 PFDs (Flow Description + URL) for one Application ID -- `Cause=RequestAccepted`,
UPF's own log independently confirms `"provisioned 2 PFD(s) for Application ID app-exampleapp"`;
(2) delete that Application ID's PFDs (PFD child IE absent) -- UPF's own log confirms `"deleted
all PFDs for Application ID app-exampleapp"`; (3) clear-all (top-level Application ID's PFDs IE
absent) -- UPF's own log confirms `"carried no Application ID's PFDs -- cleared all provisioned
PFDs"`. All three sequence numbers correctly echoed.

Full `conformance_tests`: **317/317 pass** (up from 310, the 7 new `PfcpPfdIes.*` tests), zero
regressions, same exclusions as ADR-0084/0085 (`UdrIntegration.AmfContextLifecycle`,
`UdmIntegration.SdmDataRetrievalAndSubscriptions`, both pre-existing, unrelated).

### What this ADR does NOT include

`NodeReport` (needs a new `User Plane Path Failure Report` IE, UPF-initiated not CP-initiated -- a
different message direction than every PFCP handler this project has built, including this one) and
`SessionSetDeletion` (needs a new `FQ-CSID` IE and the real bulk-cleanup-by-CP-instance semantics
ADR-0084 already declined to fake on plain Association Release) remain open, task #107's own
continuing scope -- each still needs its own spec-reading pass before implementation.

