#pragma once

#include <cstdint>

// Real Diameter command codes and base/DCC AVP codes -- P4.5/ADR-0059 Stage 1. Every constant
// below is read directly from freeDiameter's own real, vendored dictionary source
// (simulators/reference/freeDiameter/, commit e48fd4f8afc48f5e839558a90ef5a67165e94fad, BSD
// licensed -- see ADR-0059 for the license check), never invented. Each constant cites its exact
// source file.

namespace diameter_core::dictionary {

// Command codes -- simulators/reference/freeDiameter/libfdcore/dict_base_proto.c (RFC 6733 base
// protocol) and .../extensions/dict_dcca/dict_dcca.c (RFC 4006 Credit-Control Application).
namespace Command {
constexpr std::uint32_t kCapabilitiesExchange = 257; // dict_base_proto.c:2604
constexpr std::uint32_t kDeviceWatchdog = 280;       // dict_base_proto.c:2791
constexpr std::uint32_t kDisconnectPeer = 282;       // dict_base_proto.c:2714
constexpr std::uint32_t kCreditControl = 272;        // dict_dcca.c:1353 (RFC 4006, real Gy command)
constexpr std::uint32_t kAccounting = 271; // dict_base_proto.c:3236 (RFC 6733 base ACR/ACA, real Rf
                                           // command -- TS 32.299 Rf runs the Diameter Base
                                           // Accounting application over this same real command)
constexpr std::uint32_t kSessionTermination = 275; // dict_base_proto.c:2993 (RFC 6733 base STR/STA
                                                   // -- TS 29.219 Sy's own Final Spending Limit
                                                   // Report Request/Response, real command reuse)
constexpr std::uint32_t kSpendingLimit = 8388635;  // TS 29.219 §5.6.1/5.6.2 (real Sy SLR/SLA)
// ADR-0447 (increment 2 of ADR-0445/0446): dict_base_proto.c:2875/2933, RFC 6733 §8.3 RAR/RAA --
// same literal 258 as Avp::kAuthApplicationId below, a different namespace, real, not a typo (same
// disclosed pattern as kHostIpAddress's own collision with kCapabilitiesExchange above).
constexpr std::uint32_t kReAuth = 258;
} // namespace Command

// Base protocol AVP codes -- dict_base_proto.c. All base AVPs carry AVP_FLAG_VENDOR |
// AVP_FLAG_MANDATORY as their *fixed flags mask* in the real dictionary (meaning the V bit must
// always be explicitly set-or-cleared, not that V is always set) -- Vendor-Id is 0/absent for
// every one of these (they are IETF base AVPs, not vendor-specific).
namespace Avp {
constexpr std::uint32_t kSessionId = 263;        // dict_base_proto.c:1774
constexpr std::uint32_t kOriginHost = 264;       // dict_base_proto.c:548
constexpr std::uint32_t kVendorId = 266;         // dict_base_proto.c:370
constexpr std::uint32_t kFirmwareRevision = 267; // dict_base_proto.c:396
constexpr std::uint32_t kResultCode = 268;       // dict_base_proto.c:1109
constexpr std::uint32_t kProductName = 269;      // dict_base_proto.c:469
constexpr std::uint32_t kDisconnectCause = 273;  // dict_base_proto.c:513
constexpr std::uint32_t kOriginStateId = 278;    // dict_base_proto.c:2050
constexpr std::uint32_t kErrorMessage = 281;     // dict_base_proto.c:1493
constexpr std::uint32_t kDestinationRealm = 283; // dict_base_proto.c:632
constexpr std::uint32_t kDestinationHost = 293;  // dict_base_proto.c:600
constexpr std::uint32_t kOriginRealm = 296;      // dict_base_proto.c:571
constexpr std::uint32_t kHostIpAddress =
    257; // dict_base_proto.c:420 (same numeric value as CER/CEA's command code -- different
         // namespace, real, not a typo)
constexpr std::uint32_t kAuthApplicationId = 258;           // dict_base_proto.c:750
constexpr std::uint32_t kAcctApplicationId = 259;           // dict_base_proto.c:774
constexpr std::uint32_t kVendorSpecificApplicationId = 260; // dict_base_proto.c:882
constexpr std::uint32_t kSupportedVendorId = 265;           // dict_base_proto.c:447
constexpr std::uint32_t kInbandSecurityId = 299;            // dict_base_proto.c:817
constexpr std::uint32_t kAccountingRecordType = 480;        // dict_base_proto.c:2312 (Enumerated)
constexpr std::uint32_t kAccountingRecordNumber = 485;      // dict_base_proto.c:2388 (Unsigned32)
constexpr std::uint32_t kTerminationCause = 295;            // dict_base_proto.c:2010 (Enumerated)
constexpr std::uint32_t kExperimentalResult = 297;          // dict_base_proto.c:1640 (Grouped)
constexpr std::uint32_t kExperimentalResultCode = 298;      // dict_base_proto.c:1606 (Unsigned32)
// ADR-0447: dict_base_proto.c:1877, RFC 6733 §8.17. Mandatory in a real RAR (Command::kReAuth).
constexpr std::uint32_t kReAuthRequestType = 285;
} // namespace Avp

// Re-Auth-Request-Type real enumerated values -- dict_base_proto.c:1871-1872, RFC 6733 §8.17.
namespace ReAuthRequestType {
constexpr std::int32_t kAuthorizeOnly = 0;
constexpr std::int32_t kAuthorizeAuthenticate = 1;
} // namespace ReAuthRequestType

// Termination-Cause real enumerated values -- dict_base_proto.c:1998-2006. Only DIAMETER_LOGOUT is
// consumed (TS 29.219's own Table 4.5.3.1/1: "It shall be set to DIAMETER_LOGOUT" for Sy's Final
// Spending Limit Report Request) -- the other seven are real, cited values, not invented, even
// though unused today.
namespace TerminationCause {
constexpr std::int32_t kDiameterLogout = 1;
} // namespace TerminationCause

// Real Diameter Base Accounting application (RFC 6733 §9, dict_base_proto.c:107: "Diameter Base
// Accounting") -- distinct real Application-Id from RFC 4006 DCC's own Dcc::kApplicationId=4
// below. TS 32.299's Rf reference point runs this same real base-protocol accounting application,
// not a 3GPP-specific one.
namespace BaseAccounting {
constexpr std::uint32_t kApplicationId = 3; // dict_base_proto.c:107

// Accounting-Record-Type real enumerated values -- dict_base_proto.c:2304-2308.
namespace AccountingRecordType {
constexpr std::int32_t kEvent = 1;
constexpr std::int32_t kStart = 2;
constexpr std::int32_t kInterim = 3;
constexpr std::int32_t kStop = 4;
} // namespace AccountingRecordType
} // namespace BaseAccounting

// Real Result-Code enumerated values -- include/libfdproto.h (the #define macros
// dict_base_proto.c's own real DIAMETER_SUCCESS/DIAMETER_MISSING_AVP enum registrations use).
namespace ResultCode {
constexpr std::int32_t kDiameterSuccess = 2001;          // libfdproto.h:1854
constexpr std::int32_t kDiameterUnknownSessionId = 5002; // libfdproto.h:1873
constexpr std::int32_t kDiameterMissingAvp = 5005;       // libfdproto.h:1876
constexpr std::int32_t kDiameterUnableToComply = 5012;   // libfdproto.h:1883
// ADR-0291: the real transient-failure code for overload. 3xxx = protocol error a peer should
// retry/failover on, unlike 5xxx permanent failures above.
constexpr std::int32_t kDiameterTooBusy = 3004; // libfdproto.h:1860
// RFC 4006 §9.9 extended result codes, registered by dict_dcca.c against the base Result-Code
// enumerated type -- extensions/dict_dcca/dict_dcca.c:85-104 (not in libfdproto.h, which only
// carries the base-protocol values; DCC's own extension registers these at dictionary load time).
constexpr std::int32_t kDiameterEndUserServiceDenied = 4010; // dict_dcca.c:85
constexpr std::int32_t kDiameterCreditLimitReached = 4012;   // dict_dcca.c:95
constexpr std::int32_t kDiameterUserUnknown = 5030;  // dict_dcca.c:100 (RFC 4006 §9.9; also
                                                     // reused by TS 29.219 §5.5.2 for Sy)
constexpr std::int32_t kDiameterRatingFailed = 5031; // dict_dcca.c:105
} // namespace ResultCode

// RFC 4006 Diameter Credit-Control (DCC) AVP codes -- dict_dcca.c. This is the base DCC
// application (Auth-Application-Id 4); 3GPP's own Ro/Rf/Gy AVPs (TS 32.299) are a further
// extension on top of these, vendored separately at
// simulators/reference/freeDiameter/extensions/dict_dcca_3gpp/dict_dcca_3gpp.c and not yet
// consumed by this Stage 1 codec (Stage 3's own deliverable, per ADR-0059).
namespace Dcc {
// RFC 4006 §1 (quoted verbatim in dict_dcca.c:1257-1258): "The Auth-Application-Id MUST be set to
// the value 4, indicating the Diameter credit-control application."
constexpr std::uint32_t kApplicationId = 4;

constexpr std::uint32_t kCcRequestNumber = 415;               // dict_dcca.c:182
constexpr std::uint32_t kCcRequestType = 416;                 // dict_dcca.c:207
constexpr std::uint32_t kCcServiceSpecificUnits = 417;        // dict_dcca.c:230
constexpr std::uint32_t kCcTotalOctets = 421;                 // dict_dcca.c:307
constexpr std::uint32_t kRatingGroup = 432;                   // dict_dcca.c:582
constexpr std::uint32_t kFinalUnitIndication = 430;           // dict_dcca.c:967
constexpr std::uint32_t kGrantedServiceUnit = 431;            // dict_dcca.c:1109 (Grouped)
constexpr std::uint32_t kSubscriptionId = 443;                // dict_dcca.c:972 (Grouped)
constexpr std::uint32_t kRequestedServiceUnit = 437;          // dict_dcca.c:1160 (Grouped)
constexpr std::uint32_t kMultipleServicesCreditControl = 456; // dict_dcca.c:1216 (Grouped)
constexpr std::uint32_t kServiceContextId = 461;              // dict_dcca.c:694
constexpr std::uint32_t kUsedServiceUnit = 446;               // dict_dcca.c:1187 (Grouped)
constexpr std::uint32_t kValidityTime = 448;                  // dict_dcca.c:906
constexpr std::uint32_t kSubscriptionIdData = 444;            // dict_dcca.c:754 (UTF8String)
constexpr std::uint32_t kSubscriptionIdType = 450;            // dict_dcca.c:774 (Enumerated)

// CC-Request-Type real enumerated values (RFC 4006 §8.7).
namespace CcRequestType {
constexpr std::int32_t kInitial = 1;
constexpr std::int32_t kUpdate = 2;
constexpr std::int32_t kTermination = 3;
constexpr std::int32_t kEvent = 4;
} // namespace CcRequestType

// Subscription-Id-Type real enumerated values -- dict_dcca.c:772-775. Only END_USER_IMSI is
// consumed by this project's own CCR decoder (chf's real SUPI convention is IMSI-based, see
// nfs/udm/src/main.cpp's seeded subscribers) -- the other three are real, cited values, not
// invented, even though unused today.
namespace SubscriptionIdType {
constexpr std::int32_t kEndUserE164 = 0;
constexpr std::int32_t kEndUserImsi = 1;
constexpr std::int32_t kEndUserSipUri = 2;
constexpr std::int32_t kEndUserNai = 3;
} // namespace SubscriptionIdType
} // namespace Dcc

// TS 29.219 Sy reference point (spending-limit reporting, PCRF<->OCS) -- P4.5/ADR-0059 Stage 4 (Sy
// half). Real, primary spec text read directly from the user-provided ETSI TS 129 219 V19.0.0 PDF
// (specs/ts_129219v190000p.pdf, Release 19 -- matches this project's own REL-19 scope), NOT
// freeDiameter material: a direct check of the vendored simulators/reference/freeDiameter/ tree
// confirmed no dict_sy-equivalent file exists there at all (freeDiameter's own upstream does not
// ship a stock Sy dictionary the way it does for DCC/DCC-3GPP) -- this is this project's first
// Diameter AVP dictionary sourced directly from a real 3GPP spec PDF rather than freeDiameter's own
// C source, a different but still-primary citation class, disclosed rather than silently treated
// the same as the freeDiameter-sourced constants above. Each constant below cites its exact real
// clause.
namespace Sy {
constexpr std::uint32_t kVendorId = 10415;         // §5.1.5 (real 3GPP vendor ID)
constexpr std::uint32_t kApplicationId = 16777302; // §5.1.5 (real Sy application identifier)

constexpr std::uint32_t kPolicyCounterIdentifier = 2901;         // §5.3.1, UTF8String
constexpr std::uint32_t kPolicyCounterStatus = 2902;             // §5.3.2, UTF8String
constexpr std::uint32_t kPolicyCounterStatusReport = 2903;       // §5.3.3, Grouped
constexpr std::uint32_t kSlRequestType = 2904;                   // §5.3.4, Enumerated
constexpr std::uint32_t kPendingPolicyCounterInformation = 2905; // §5.3.5, Grouped -- not consumed
                                                                 // (no real pending-status engine
                                                                 // exists in this codebase, same
                                                                 // category of gap as CHF's own
                                                                 // fixed "unknown" currentStatus,
                                                                 // main.cpp's own disclosure)
constexpr std::uint32_t kPendingPolicyCounterChangeTime = 2906;  // §5.3.6, Time -- not consumed
constexpr std::uint32_t kSnRequestType = 2907; // §5.3.7, Unsigned32 (ASR feature) -- not consumed;
                                               // this Stage only handles PCRF-initiated SLR/STR,
                                               // not OCS-initiated SNR (see this ADR's own
                                               // disclosure for why)

// SL-Request-Type real enumerated values -- §5.3.4.
namespace SlRequestType {
constexpr std::int32_t kInitial = 0;
constexpr std::int32_t kIntermediate = 1;
} // namespace SlRequestType

// Sy-specific Experimental-Result-Code values -- §5.5.2 (permanent)/§5.5.3 (transient), carried in
// the real base-protocol Experimental-Result grouped AVP (Avp::kExperimentalResult=297) alongside
// Vendor-Id=kVendorId above.
namespace ExperimentalResultCode {
constexpr std::int32_t kUnknownPolicyCounters = 5570;     // §5.5.2, permanent
constexpr std::int32_t kNoAvailablePolicyCounters = 4241; // §5.5.3, transient -- not emitted by
                                                          // this codec (CHF always has "available"
                                                          // policy counters in the trivial sense
                                                          // that it accepts any policyCounterId,
                                                          // see build_spending_limit_status's own
                                                          // disclosed "unknown" placeholder)
} // namespace ExperimentalResultCode
} // namespace Sy

} // namespace diameter_core::dictionary
