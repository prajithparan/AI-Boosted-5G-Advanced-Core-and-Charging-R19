#pragma once

#include <cstdint>
#include <vector>

// Real MAP (Mobile Application Part) constants -- 3GPP TS 29.002 V19.0.0, fetched directly from
// ETSI's own /deliver/ portal (specs/ts_129002v190000p.pdf, not committed -- ETSI-copyrighted).
// P4.5/ADR-0059 Stage 7 (MAP) kickoff.
//
// Real, disclosed scope: MAP is a genuinely huge protocol (clause 17, ~170 pages, 25 ASN.1
// modules, ~90+ operations covering mobility management, call handling, SS, SMS, group-call,
// LCS). This first increment covers exactly one real operation --
// insertSubscriberData (TS 29.002 clause 17.6.1, page 358) -- chosen because it is the real
// mechanism by which an HLR provisions CAMEL Subscription Info (O-CSI/D-CSI) to a VLR/MSC, which
// is what causes a real switch to later invoke CAP's InitialDP (already built, libs/cap-core) --
// closing the real architectural loop between the two protocols. All other MAP operations are
// out of scope for this increment; their real opcodes (cited below where already found during
// research) are NOT wired to any argument codec yet.
//
// 2026-08-15: the numeric Application Context OID gap noted above is now resolved with real,
// cross-checked evidence (still not fabricated -- two independent real sources agree). TS 29.002
// clause 17.3.2.17 gives the symbolic name (subscriberDataMngt(16) version3(3)); TS 29.002's own
// module text (grep-confirmed in specs/ts_129002v190000p.pdf) fixes the numeric prefix `map-ac
// OBJECT IDENTIFIER ::= {gsm-NetworkId ac-Id}` under `gsm-Network(1)` = `{itu-t(0)
// identified-organization(4) etsi(0) mobileDomain(0) gsm-Network(1)}` = 0.4.0.0.1 (this exact
// 5-arc prefix is independently confirmed already in use by libs/cap-core's own
// `kGsmssfScfGenericAcOid`, a sibling arc under the same real gsm-Network(1) node). The remaining
// unresolved piece -- `ac-Id`'s own numeric value -- is confirmed as `0` directly from RestComm
// jss7's own real, working `MAPApplicationContext.java` (simulators/reference/jss7/ pinned commit,
// arms-length reference only, same license-checked treatment as every other jss7 fact in this
// project): its `oidTemplate`/`res` arrays are `{0, 4, 0, 0, 1, 0, 0, 0}` with only index 6
// (context code) and index 7 (version) ever written by real running code -- i.e. `ac-Id` is
// literally 0 in every real MAP application context OID jss7 constructs. Combined:
// subscriberDataMngtContext-v3 = {0, 4, 0, 0, 1, 0, 16, 3}.

namespace map_core {

// Real Application Context Name OID for insertSubscriberData's own real dialogue (TS 29.002
// clause 17.3.2.17 symbolic name + jss7 cross-check for the numeric `ac-Id` arc -- see the header
// comment above for the full real derivation).
inline const std::vector<std::uint32_t> kSubscriberDataMngtContextV3Oid = {0, 4, 0, 0, 1, 0, 16, 3};

// Real Application Context Name OID for cancelLocation's own dialogue (ADR-0296).
// jss7's `MAPApplicationContextName` gives the ac-Id arc directly --
// `locationCancellationContext(2)` -- and `MAPApplicationContext` builds every AC OID from the
// template `{0, 4, 0, 0, 1, 0, <ac-Id>, <version>}`. That template independently reproduces the
// constant above from `subscriberDataMngtContext(16)`, which is what makes it trustworthy here
// rather than a pattern guessed from a single example.
inline const std::vector<std::uint32_t> kLocationCancellationContextV3Oid = {
    0, 4, 0, 0, 1, 0, 2, 3};

// Real operation local codes -- TS 29.002 clause 17.6.1 (MAP-MobileServiceOperations), each cited
// with its exact CODE local:N as printed on the page. Only insertSubscriberData has an argument
// codec in map_operations.hpp; the rest are recorded as real, cited facts for future stages.
namespace Opcode {
constexpr std::int32_t kUpdateLocation = 2;
constexpr std::int32_t kCancelLocation = 3;
constexpr std::int32_t kInsertSubscriberData = 7;
constexpr std::int32_t kDeleteSubscriberData = 8;
constexpr std::int32_t kCheckImei = 43;
constexpr std::int32_t kSendIdentification = 55;
constexpr std::int32_t kSendAuthenticationInfo = 56;
constexpr std::int32_t kPurgeMs = 67;
} // namespace Opcode

// Real error names used by insertSubscriberData (TS 29.002 clause 17.6.1, page 358: "ERRORS {
// dataMissing | unexpectedDataValue | unidentifiedSubscriber }"). Real, disclosed gap: the
// numeric local error codes for these (module 12, MAP-Errors) were not located during this
// increment's research pass -- clause 17.6.9 (where MAP-Errors' operations-clause slot would sit,
// by the module-number ordering TS 29.002 clause 17.1 itself documents) is marked "Void" in this
// V19.0.0 text, so the numeric definitions live somewhere else not yet found. Not fabricated: no
// numeric constants are declared for these until located.

} // namespace map_core
