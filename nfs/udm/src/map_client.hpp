#pragma once

#include <cstdint>
#include <string>

#include "map_core/map_operations.hpp"

// Private to nfs/udm -- not shared with any other NF, per CLAUDE.md's "no NF includes another NF's
// private headers" rule.
//
// P4.5/ADR-0061: UDM's real MAP client capability, playing the real HLR role toward a real VLR/MSC
// peer (SSN kVlr) -- the CONSUMER side of `subscriberDataMngtStandAlonePackage-v3`
// (TS 29.002 clause 17.2.2.15: "-- Supplier is VLR or SGSN if Consumer is HLR or CSS, CONSUMER
// INVOKES { insertSubscriberData }"), confirming the real direction: the HLR (UDM, here) INITIATES
// insertSubscriberData toward the VLR, not the other way around -- so this is a client capability,
// not a listener, unlike CHF's Diameter server (Diameter Gy has CHF as the real server role; MAP's
// insertSubscriberData has UDM as the real client/consumer role).
//
// Real transport chain, each layer already real and tested in isolation before this file composed
// them (libs/ss7-core, libs/tcap-core, libs/map-core): kernel SCTP (RFC 4960, client `connect()`,
// ss7_core::SctpSocket) -> M3UA ASPSM/ASPTM activation handshake (RFC 4666 §3.5/§3.7) -> M3UA DATA
// message carrying a real SCCP UDT (ITU-T Q.713, addressed calling=HLR/called=VLR by real SSN) ->
// a real TCAP TC-Begin carrying one real Invoke (insertSubscriberData) -> the peer's real
// TC-End/TC-Continue carrying a real ReturnResultLast/ReturnResult (or ReturnError/Reject, both
// real failure outcomes this function surfaces as `false`).
//
// Real, disclosed scope: this is a synchronous, blocking, single-dialogue call (opens a fresh SCTP
// association, does the full handshake, sends exactly one Invoke, waits for exactly one response,
// then the association is torn down on return) -- not a persistent, multiplexed
// association/dialogue-pool the way CHF's real DiameterServer keeps long-lived peer connections.
// Real, disclosed gap: NOT yet called from any real UDM procedure -- the real trigger event (a
// real MAP `updateLocation` received FROM a VLR, which is what causes a real HLR to respond with
// `insertSubscriberData`) has no receive-side implementation in this codebase yet. Live-verified
// end-to-end against a standalone test "VLR" peer (see this update's own docs/DECISIONS.md ADR-0061
// entry for the real verification evidence), not yet exercised against a real deployed VLR/MSC.

namespace udm {

// Opens a real SCTP association to peer_address:peer_port, performs the real M3UA ASPSM/ASPTM
// handshake, sends a real insertSubscriberData Invoke carrying `arg`, and waits for the peer's
// real response. Returns true only on a real, successfully decoded InsertSubscriberDataRes
// (ReturnResult/ReturnResultLast) -- false on any transport failure, protocol-level rejection
// (ReturnError/Reject), or malformed response.
bool send_insert_subscriber_data(const std::string& peer_address,
                                 std::uint16_t peer_port,
                                 const map_core::InsertSubscriberDataArg& arg);

// ADR-0296: the counterpart operation -- tells a VLR to drop a subscriber it no longer serves.
// Same real dialogue shape as above, with cancelLocation's own real application context
// (locationCancellationContext-v3, not subscriberDataMngtContext). Same return contract: true only
// on a real, successfully decoded CancelLocationRes.
bool send_cancel_location(const std::string& peer_address,
                          std::uint16_t peer_port,
                          const map_core::CancelLocationArg& arg);

} // namespace udm
