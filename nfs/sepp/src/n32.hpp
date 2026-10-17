#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "TS26510_CommonData_grp.hpp"

// The SEPP's N32 logic that does not touch a socket (ADR-0491): PLMN domains (TS 23.003 28.2 /
// 28.5), the responding SEPP's Security Capability Negotiation decision (TS 29.573 5.2.2), and the
// correlation of an incoming N32-f request to its N32-c context when TLS is the negotiated
// security (TS 29.573 5.3.3.2). Private to nfs/sepp.

namespace sepp {

// TS 23.003 28.2: "5gc.mnc<MNC>.mcc<MCC>.3gppnetwork.org", MNC padded to three digits.
std::string home_network_domain(const std::string& mcc, const std::string& mnc);

// The host part of an apiRoot / URI / authority ("https://h:443/x" -> "h"), lower-cased.
std::string host_of(std::string_view uri_or_authority);

// The Home Network Domain a host ends with (TS 23.003 28.5.1: "the FQDN of the target NF shall
// have the Home Network Domain ... as the trailing part"), nullopt if it has none.
std::optional<std::string> network_domain_of_host(std::string_view host);

// TS 29.573 5.3.3.3: n32HandshakeId = 16HEXDIG.
bool is_handshake_id(std::string_view s);

// A partner PLMN's SEPP, from config.
struct PeerConfig {
    std::string fqdn; // the peer SEPP's FQDN (SecNegotiateReqData.sender)
    std::string mcc;  // the peer's PLMN
    std::string mnc;
    std::string n32_api_root; // where its N32-c / N32-f server is reached
    bool initiate = false;    // this SEPP starts the handshake towards it
};

// One N32-c context with TLS security negotiated.
struct Context {
    std::string security = "TLS"; // the negotiated SecurityCapability: "TLS" or "PRINS"
    std::string peer_fqdn;
    std::string peer_domain; // the peer PLMN's Home Network Domain
    std::string n32_api_root;
    std::optional<std::string> handshake_id;
    bool peer_target_api_root = false; // peer indicated 3gpp-Sbi-Target-apiRoot support
    std::vector<std::string> purposes;
};

class ContextStore {
public:
    void put(Context c);
    bool erase(const std::string& peer_fqdn);
    std::optional<Context> by_peer(const std::string& peer_fqdn) const;
    std::optional<Context> by_domain(const std::string& domain) const;
    std::vector<Context> all() const;
    // 5.2.2 collision handling: a request this SEPP has sent and not yet had answered.
    void set_outgoing(const std::string& peer_fqdn, bool in_progress);
    bool outgoing(const std::string& peer_fqdn) const;

private:
    mutable std::mutex mutex_;
    std::vector<Context> contexts_;
    std::set<std::string> outgoing_;
};

struct NegotiationPolicy {
    std::string own_fqdn;
    std::string own_mcc;
    std::string own_mnc;
    std::vector<std::string> allowed_purposes;
    std::vector<PeerConfig> peers;
    // Local preference order (5.2.2 "selects, based on its local policy"); offered as-is.
    std::vector<std::string> security_capabilities = {"TLS"};
    // TS 33.210 6.3.2: A128GCM shall, A256GCM should be supported; JWS ES256 only (13.2.4.9).
    std::vector<std::string> jwe_cipher_suites = {"A128GCM", "A256GCM"};
    std::vector<std::string> jws_cipher_suites = {"ES256"};
    sbi_gen::ProtectionPolicy protection_policy; // this SEPP's, sent in 5.2.3.3
};

// What the responding SEPP answers to an exchange-capability request (TS 29.573 5.2.2 step 2a/2b,
// errors per table 6.1.6.3-1 and TS 29.500 table 5.2.7.2-1).
struct Decision {
    int status = 200;
    std::optional<sbi_gen::SecNegotiateRspData> response;
    std::optional<std::string> cause;
    std::string detail;
    std::optional<Context> context; // to store (TLS selected)
    bool teardown = false;          // NONE: delete the context for this peer
};

Decision negotiate(const NegotiationPolicy& policy,
                   const sbi_gen::SecNegotiateReqData& request,
                   bool outgoing_in_progress);

// The request this SEPP sends as initiating SEPP (supportedSecCapabilityList: TLS, or NONE to tear
// down).
sbi_gen::SecNegotiateReqData make_request(const NegotiationPolicy& policy,
                                          const PeerConfig& peer,
                                          const std::optional<std::string>& handshake_id,
                                          bool teardown);

// The context the initiating SEPP keeps from a 200 response; nullopt if the response did not select
// TLS (this SEPP offers nothing else).
std::optional<Context> context_from_response(const PeerConfig& peer,
                                             const sbi_gen::SecNegotiateRspData& response,
                                             const std::optional<std::string>& handshake_id);

// TS 29.573 5.3.3.2.1: the N32-c context an incoming N32-f request belongs to.
// - The peer first: only contexts whose peer FQDN appears in the client certificate's dNSName
//   SANs; if the certificate names several SEPPs (a common certificate), the Via header's
//   "SEPP-<fqdn>" entries must also name the peer (the clause's NOTE). More than one peer left:
//   no context.
// - Several contexts with that one peer: by the 3gpp-Sbi-N32-Handshake-Id header when ids were
//   exchanged, else by 3gpp-Sbi-Interplmn-Purpose ("ROAMING" when absent) against each context's
//   purposes. These rules never choose between peers (SEC-02, REVIEW-5ae0ef1).
std::optional<Context> correlate(const std::vector<Context>& contexts,
                                 const std::vector<std::string>& cert_dns_names,
                                 const std::string& via,
                                 const std::optional<std::string>& handshake_id,
                                 const std::optional<std::string>& purpose);

// ---- PRINS: parameter exchange and the N32-f context (TS 29.573 5.2.3, TS 33.501 13.2.2;
// ADR-0493)
// ----------------------------------------------------------------------------------------

// TS 33.501 13.2.4.4.1: label of the TLS exporter giving the 64-octet N32-f master key.
inline constexpr const char* kN32MasterExporterLabel = "EXPORTER_3GPP_N32_MASTER";
inline constexpr std::size_t kN32MasterKeyLength = 64;

struct PrinsContext {
    std::string peer_fqdn;
    std::string local_id;   // the n32fContextId this SEPP issued (the peer uses it towards us)
    std::string peer_id;    // the n32fContextId the peer issued (we use it towards the peer)
    bool initiator = false; // this SEPP initiated the N32-c connection (the "parallel" side)
    std::string jwe_cipher;
    std::optional<std::string> jws_cipher;
    std::vector<std::uint8_t> master_key;
    std::optional<sbi_gen::ProtectionPolicy> peer_policy;     // what the peer sent (5.2.3.3)
    std::optional<sbi_gen::ProtectionPolicy> selected_policy; // what the responder selected
};

class PrinsStore {
public:
    void put(PrinsContext c);
    std::optional<PrinsContext> by_peer(const std::string& peer_fqdn) const;
    std::optional<PrinsContext> by_local_id(const std::string& id) const;
    bool erase_local_id(const std::string& id);
    std::vector<PrinsContext> all() const;
    void set_outgoing(const std::string& peer_fqdn, bool in_progress);
    bool outgoing(const std::string& peer_fqdn) const;

private:
    mutable std::mutex mutex_;
    std::vector<PrinsContext> contexts_;
    std::set<std::string> outgoing_;
};

// A random n32fContextId ('^[A-Fa-f0-9]{16}$'; TS 33.501 13.2.2.4.1: "a random value").
std::string random_n32f_context_id();

// RFC 5869 HKDF-Expand (SHA-256), as N32-KDF uses it.
std::vector<std::uint8_t>
hkdf_expand(const std::vector<std::uint8_t>& prk, const std::string& info, std::size_t length);

// TS 33.501 13.2.4.4.1: N32-KDF(label, L) = HKDF-Expand(master, "N32" || N32-Context-ID || label,
// L).
std::vector<std::uint8_t> n32_kdf(const std::vector<std::uint8_t>& master_key,
                                  const std::string& n32f_context_id,
                                  const std::string& label,
                                  std::size_t length);

// A short non-reversible fingerprint of the master key, logged by both SEPPs so key agreement can
// be confirmed without the key ever being logged.
std::string key_fingerprint(const std::vector<std::uint8_t>& key);

struct ParamDecision {
    int status = 200;
    std::optional<sbi_gen::SecParamExchRspData> response;
    std::optional<std::string> cause;
    std::string detail;
    std::optional<PrinsContext> context; // to store
};

// The responding SEPP's answer to exchange-params (5.2.3.2 cipher suites / 5.2.3.3 protection
// policy). `capability` is the N32-c context with that peer, `existing` its PRINS context if any,
// `master_key` the exporter output of the TLS connection the request arrived on.
ParamDecision exchange_params(const NegotiationPolicy& policy,
                              const std::optional<Context>& capability,
                              const std::optional<PrinsContext>& existing,
                              const sbi_gen::SecParamExchReqData& request,
                              const std::optional<std::vector<std::uint8_t>>& master_key,
                              bool outgoing_in_progress);

// ---- Telescopic FQDN (TS 23.003 28.5.2, TS 29.573 5.4, TS 29.500 6.1.4.3.2; ADR-0492) ----------

// The single label replacing a foreign FQDN: "tel-" + 16 hex digits of SHA-256(lower-case FQDN).
// TS 23.003 28.5.2 NOTE 1 leaves the construction to the implementation; a hash makes every SEPP
// replica derive the same label without sharing state, and stays well inside 63 octets.
std::string telescopic_label(const std::string& foreign_fqdn);

// Labels this SEPP has handed out (5.4.2), resolvable back to the foreign FQDN (5.4.3).
class TelescopicTable {
public:
    std::string label_for(const std::string& foreign_fqdn); // records the mapping
    std::optional<std::string> foreign_for(const std::string& label) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> by_label_;
};

// "<label>.<sepp_fqdn>" -> the label (one DNS label, nothing else in front), else nullopt.
std::optional<std::string> telescopic_label_of_host(std::string_view host,
                                                    const std::string& sepp_fqdn);

} // namespace sepp
