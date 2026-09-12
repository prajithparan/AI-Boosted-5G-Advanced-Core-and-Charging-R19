#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>

// Header names transcribed verbatim from specs/5G_APIs-REL-19/TS29500_CustomHeaders.abnf
// (3GPP TS 29.500 v19.5.0, December 2025). Only the header names are exhaustive here; typed
// builders/parsers for the full ABNF grammar of each header are added incrementally as the NF
// procedures that actually use them are implemented (Phase 2+), per the "no speculative
// abstraction" rule in CLAUDE.md. Sender-Timestamp and Producer-Id are implemented now because
// Phase 0's hello-nf <-> stub-nrf flow uses them directly.

namespace sbi_core::headers {

inline constexpr const char* kMessagePriority = "3gpp-Sbi-Message-Priority";
inline constexpr const char* kCallback = "3gpp-Sbi-Callback";
inline constexpr const char* kTargetApiRoot = "3gpp-Sbi-Target-apiRoot";
inline constexpr const char* kScpApiRoot = "3gpp-Sbi-Scp-apiRoot";
inline constexpr const char* kRoutingBinding = "3gpp-Sbi-Routing-Binding";
inline constexpr const char* kBinding = "3gpp-Sbi-Binding";
inline constexpr const char* kProducerId = "3gpp-Sbi-Producer-Id";
inline constexpr const char* kOci = "3gpp-Sbi-Oci";
inline constexpr const char* kLci = "3gpp-Sbi-Lci";
inline constexpr const char* kClientCredentials = "3gpp-Sbi-Client-Credentials";
inline constexpr const char* kSourceNfClientCredentials = "3gpp-Sbi-Source-NF-Client-Credentials";
inline constexpr const char* kNrfUri = "3gpp-Sbi-Nrf-Uri";
inline constexpr const char* kTargetNfId = "3gpp-Sbi-Target-Nf-Id";
inline constexpr const char* kMaxForwardHops = "3gpp-Sbi-Max-Forward-Hops";
inline constexpr const char* kOriginatingNetworkId = "3gpp-Sbi-Originating-Network-Id";
inline constexpr const char* kAccessScope = "3gpp-Sbi-Access-Scope";
inline constexpr const char* kOtherAccessScopes = "3gpp-Sbi-Other-Access-Scopes";
inline constexpr const char* kAccessToken = "3gpp-Sbi-Access-Token";
inline constexpr const char* kTargetNfGroupId = "3gpp-Sbi-Target-Nf-Group-Id";
inline constexpr const char* kNrfUriCallback = "3gpp-Sbi-Nrf-Uri-Callback";
inline constexpr const char* kNfPeerInfo = "3gpp-Sbi-NF-Peer-Info";
inline constexpr const char* kSenderTimestamp = "3gpp-Sbi-Sender-Timestamp";
inline constexpr const char* kMaxRspTime = "3gpp-Sbi-Max-Rsp-Time";
inline constexpr const char* kCorrelationInfo = "3gpp-Sbi-Correlation-Info";
inline constexpr const char* kAlternateChfId = "3gpp-Sbi-Alternate-Chf-Id";
inline constexpr const char* kNotifAcceptedEncoding = "3gpp-Sbi-Notif-Accepted-Encoding";
inline constexpr const char* kConsumerInfo = "3gpp-Sbi-Consumer-Info";
inline constexpr const char* kResponseInfo = "3gpp-Sbi-Response-Info";
inline constexpr const char* kSelectionInfo = "3gpp-Sbi-Selection-Info";
inline constexpr const char* kInterplmnPurpose = "3gpp-Sbi-Interplmn-Purpose";
inline constexpr const char* kRequestInfo = "3gpp-Sbi-Request-Info";
inline constexpr const char* kRetryInfo = "3gpp-Sbi-Retry-Info";
inline constexpr const char* kBindingIndicationNotifyRestricted =
    "3gpp-Sbi-Binding-Indication-Notify-Restricted";

// Sbi-Sender-Timestamp-Header = day-name "," SP date1 SP time-of-day "." milliseconds SP "GMT"
// e.g. "Tue, 04 Aug 2026 15:04:05.123 GMT"
std::string format_sender_timestamp(std::chrono::system_clock::time_point tp);

// Sbi-Producer-Id-Header = "nfinst=" nfinst [";nfservinst=" nfservinst]
//                          [";nfset=" nfset] [";nfserviceset=" nfserviceset]
struct ProducerId {
    std::string nf_instance_id;
    std::optional<std::string> nf_service_instance_id;
    std::optional<std::string> nf_set_id;
    std::optional<std::string> nf_service_set_id;
};

std::string format_producer_id(const ProducerId& id);
std::optional<ProducerId> parse_producer_id(const std::string& header_value);

// TS 29.500 clause 5.2.3.3.12, real ABNF:
//   Sbi-Request-Info-Header = "3gpp-Sbi-Request-Info:" OWS req-param *( ";" OWS req-param ) OWS
//   req-param              = req-param-name "=" OWS req-param-value
//   req-param-name         = "retrans" / "redirect" / "reason" / "idempotency-key" /
//                            "receivedrejectioncause" / "callback-uri-prefix" / "nfinst" /
//                            "nfservinst" / "redirection-cause" / token
//
// `token` is in the name production, so an unknown parameter is legal and must not invalidate the
// header -- every parameter is returned and the caller takes the ones it understands.
std::map<std::string, std::string> parse_request_info(const std::string& header_value);

// Clause 5.2.3.3.12: "it is a string and may be encoded using Universally Unique Identifier
// (UUID) ... to uniquely identify a request message (to be received) in the target NF."
// Returns the idempotency-key parameter if the header carries one.
std::optional<std::string> request_info_idempotency_key(const std::string& header_value);

} // namespace sbi_core::headers
