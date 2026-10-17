#include "n32.hpp"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

namespace sepp {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const PeerConfig* find_peer(const NegotiationPolicy& policy, const std::string& fqdn) {
    const auto it = std::ranges::find_if(
        policy.peers, [&](const PeerConfig& p) { return lower(p.fqdn) == lower(fqdn); });
    return it == policy.peers.end() ? nullptr : &*it;
}

Decision reject(int status, std::string cause, std::string detail) {
    Decision d;
    d.status = status;
    d.cause = std::move(cause);
    d.detail = std::move(detail);
    return d;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::ranges::find(v, s) != v.end();
}

// The FQDNs of the "SEPP-<fqdn>" entries of a Via header (TS 29.500 5.2.2.2 / 6.10.10.3: e.g.
// "2.0 SEPP-sepp1.operator.com"), lower-cased.
std::vector<std::string> via_sepps(const std::string& via) {
    std::vector<std::string> out;
    static const std::regex kEntry(R"(SEPP-([A-Za-z0-9.\-]+))");
    for (auto it = std::sregex_iterator(via.begin(), via.end(), kEntry);
         it != std::sregex_iterator();
         ++it) {
        out.push_back(lower((*it)[1].str()));
    }
    return out;
}

} // namespace

std::string home_network_domain(const std::string& mcc, const std::string& mnc) {
    return "5gc.mnc" + (mnc.size() == 2 ? "0" + mnc : mnc) + ".mcc" + mcc + ".3gppnetwork.org";
}

std::string host_of(std::string_view uri_or_authority) {
    std::string_view s = uri_or_authority;
    if (const auto scheme = s.find("://"); scheme != std::string_view::npos) {
        s.remove_prefix(scheme + 3);
    }
    s = s.substr(0, s.find('/'));
    if (const auto at = s.rfind('@'); at != std::string_view::npos) {
        s.remove_prefix(at + 1);
    }
    if (!s.empty() && s.front() == '[') { // IPv6 literal
        return lower(s.substr(0, s.find(']') + 1));
    }
    return lower(s.substr(0, s.find(':')));
}

std::optional<std::string> network_domain_of_host(std::string_view host) {
    static const std::regex kDomain(R"((^|\.)(5gc\.mnc[0-9]{3}\.mcc[0-9]{3}\.3gppnetwork\.org)$)");
    const std::string h = lower(host);
    std::smatch m;
    if (std::regex_search(h, m, kDomain)) {
        return m[2].str();
    }
    return std::nullopt;
}

bool is_handshake_id(std::string_view s) {
    return s.size() == 16 &&
           std::ranges::all_of(s, [](unsigned char c) { return std::isxdigit(c); });
}

void ContextStore::put(Context c) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(contexts_, [&](const Context& x) { return x.peer_fqdn == c.peer_fqdn; });
    contexts_.push_back(std::move(c));
}

bool ContextStore::erase(const std::string& peer_fqdn) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::erase_if(contexts_, [&](const Context& x) { return x.peer_fqdn == peer_fqdn; }) > 0;
}

std::optional<Context> ContextStore::by_peer(const std::string& peer_fqdn) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it =
        std::ranges::find_if(contexts_, [&](const Context& x) { return x.peer_fqdn == peer_fqdn; });
    return it == contexts_.end() ? std::nullopt : std::optional{*it};
}

std::optional<Context> ContextStore::by_domain(const std::string& domain) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it =
        std::ranges::find_if(contexts_, [&](const Context& x) { return x.peer_domain == domain; });
    return it == contexts_.end() ? std::nullopt : std::optional{*it};
}

std::vector<Context> ContextStore::all() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return contexts_;
}

void ContextStore::set_outgoing(const std::string& peer_fqdn, bool in_progress) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (in_progress) {
        outgoing_.insert(peer_fqdn);
    } else {
        outgoing_.erase(peer_fqdn);
    }
}

bool ContextStore::outgoing(const std::string& peer_fqdn) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return outgoing_.contains(peer_fqdn);
}

Decision negotiate(const NegotiationPolicy& policy,
                   const sbi_gen::SecNegotiateReqData& request,
                   bool outgoing_in_progress) {
    const PeerConfig* peer = find_peer(policy, request.sender);
    if (peer == nullptr) {
        // Not a configured roaming / interconnect partner: no security capability is configured
        // for it at this SEPP (table 6.1.6.3-1).
        return reject(403,
                      "NEGOTIATION_NOT_ALLOWED",
                      "no security capability is configured for SEPP " + request.sender);
    }
    if (request.targetPlmnId &&
        home_network_domain(request.targetPlmnId->mcc, request.targetPlmnId->mnc) !=
            home_network_domain(policy.own_mcc, policy.own_mnc)) {
        return reject(400, "OPTIONAL_IE_INCORRECT", "targetPlmnId is not a PLMN this SEPP serves");
    }
    if (request.n32HandshakeId && !is_handshake_id(*request.n32HandshakeId)) {
        return reject(400, "OPTIONAL_IE_INCORRECT", "n32HandshakeId must be 16 hexadecimal digits");
    }

    sbi_gen::SecNegotiateRspData rsp;
    rsp.sender = policy.own_fqdn;
    rsp.plmnIdList = std::vector<sbi_gen::PlmnId_CommonData>{{policy.own_mcc, policy.own_mnc}};

    const auto& caps = request.supportedSecCapabilityList;
    const auto has = [&](const std::string& c) {
        return std::ranges::any_of(caps, [&](const auto& x) { return x.value == c; });
    };
    // Tear-down of the N32-f TLS connection: the capability set to "NONE" (5.2.2 step 1 / 2a).
    if (has(sbi_gen::SecurityCapability::NONE)) {
        Decision d;
        rsp.selectedSecCapability.value = sbi_gen::SecurityCapability::NONE;
        d.response = rsp;
        d.teardown = true;
        return d;
    }
    // Collision: both SEPPs initiated; the one whose FQDN lexicographically precedes continues.
    if (outgoing_in_progress && lower(policy.own_fqdn) < lower(request.sender)) {
        return reject(409,
                      "N32C_EXCHANGE_CAPABILITY_ONGOING",
                      "this SEPP's own exchange-capability request to " + request.sender +
                          " is in progress and takes precedence");
    }
    // The first capability in this SEPP's own preference order that the initiator supports.
    std::string selected;
    for (const auto& c : policy.security_capabilities) {
        if (has(c)) {
            selected = c;
            break;
        }
    }
    if (selected.empty()) {
        return reject(403,
                      "NEGOTIATION_NOT_ALLOWED",
                      "no security capability offered is configured at this SEPP");
    }

    // Purposes: none exchanged means roaming and inter-PLMN mobility (5.2.2, 6.1.5.3.9).
    std::vector<std::string> requested;
    if (request.intendedUsagePurpose) {
        for (const auto& p : *request.intendedUsagePurpose) {
            requested.push_back(p.usagePurpose.value);
        }
    } else {
        requested = {sbi_gen::N32Purpose::ROAMING, sbi_gen::N32Purpose::INTER_PLMN_MOBILITY};
    }
    std::vector<std::string> allowed;
    std::vector<sbi_gen::IntendedN32Purpose> allowed_ie;
    std::vector<sbi_gen::IntendedN32Purpose> rejected_ie;
    for (const auto& p : requested) {
        sbi_gen::IntendedN32Purpose ie;
        ie.usagePurpose.value = p;
        if (contains(policy.allowed_purposes, p)) {
            allowed.push_back(p);
            allowed_ie.push_back(ie);
        } else {
            rejected_ie.push_back(ie);
        }
    }
    if (allowed.empty()) {
        return reject(403,
                      "REQUESTED_PURPOSE_NOT_ALLOWED",
                      "none of the requested N32 purposes is allowed with this SEPP");
    }
    if (request.intendedUsagePurpose) {
        rsp.allowedUsagePurpose = allowed_ie;
        if (!rejected_ie.empty()) {
            rsp.rejectedUsagePurpose = rejected_ie;
        }
    }

    rsp.selectedSecCapability.value = selected;
    if (selected == sbi_gen::SecurityCapability::TLS) {
        // "Whether the 3gpp-Sbi-Target-apiRoot HTTP header is supported, if TLS security is
        // selected" (5.2.2 step 2a).
        rsp.n3GppSbiTargetApiRootSupported = true;
    }
    rsp.n32HandshakeId = request.n32HandshakeId;

    Context ctx;
    ctx.security = selected;
    ctx.peer_fqdn = lower(peer->fqdn);
    ctx.peer_domain = home_network_domain(peer->mcc, peer->mnc);
    ctx.n32_api_root = peer->n32_api_root;
    ctx.handshake_id = request.n32HandshakeId;
    ctx.peer_target_api_root = request.n3GppSbiTargetApiRootSupported.value_or(false);
    ctx.purposes = allowed;

    Decision d;
    d.response = rsp;
    d.context = ctx;
    return d;
}

sbi_gen::SecNegotiateReqData make_request(const NegotiationPolicy& policy,
                                          const PeerConfig& peer,
                                          const std::optional<std::string>& handshake_id,
                                          bool teardown) {
    sbi_gen::SecNegotiateReqData req;
    req.sender = policy.own_fqdn;
    sbi_gen::SecurityCapability cap;
    if (teardown) {
        cap.value = sbi_gen::SecurityCapability::NONE;
        req.supportedSecCapabilityList = {cap};
    } else {
        for (const auto& c : policy.security_capabilities) {
            cap.value = c;
            req.supportedSecCapabilityList.push_back(cap);
        }
    }
    if (!teardown) {
        if (std::ranges::find(policy.security_capabilities, sbi_gen::SecurityCapability::TLS) !=
            policy.security_capabilities.end()) {
            req.n3GppSbiTargetApiRootSupported = true;
        }
        req.plmnIdList = std::vector<sbi_gen::PlmnId_CommonData>{{policy.own_mcc, policy.own_mnc}};
        req.targetPlmnId = sbi_gen::PlmnId_CommonData{peer.mcc, peer.mnc};
        std::vector<sbi_gen::IntendedN32Purpose> purposes;
        for (const auto& p : policy.allowed_purposes) {
            sbi_gen::IntendedN32Purpose ie;
            ie.usagePurpose.value = p;
            purposes.push_back(ie);
        }
        if (!purposes.empty()) {
            req.intendedUsagePurpose = purposes;
        }
    }
    req.n32HandshakeId = handshake_id;
    return req;
}

std::optional<Context> context_from_response(const PeerConfig& peer,
                                             const sbi_gen::SecNegotiateRspData& response,
                                             const std::optional<std::string>& handshake_id) {
    const auto& sel = response.selectedSecCapability.value;
    if (sel != sbi_gen::SecurityCapability::TLS && sel != sbi_gen::SecurityCapability::PRINS) {
        return std::nullopt;
    }
    Context ctx;
    ctx.security = sel;
    ctx.peer_fqdn = lower(peer.fqdn);
    ctx.peer_domain = home_network_domain(peer.mcc, peer.mnc);
    ctx.n32_api_root = peer.n32_api_root;
    // The id is shared only if the responder echoed it (it supports the N32 handshake id).
    if (handshake_id && response.n32HandshakeId == handshake_id) {
        ctx.handshake_id = handshake_id;
    }
    ctx.peer_target_api_root = response.n3GppSbiTargetApiRootSupported.value_or(false);
    if (response.allowedUsagePurpose) {
        for (const auto& p : *response.allowedUsagePurpose) {
            ctx.purposes.push_back(p.usagePurpose.value);
        }
    } else {
        ctx.purposes = {sbi_gen::N32Purpose::ROAMING, sbi_gen::N32Purpose::INTER_PLMN_MOBILITY};
    }
    return ctx;
}

std::optional<Context> correlate(const std::vector<Context>& contexts,
                                 const std::vector<std::string>& cert_dns_names,
                                 const std::string& via,
                                 const std::optional<std::string>& handshake_id,
                                 const std::optional<std::string>& purpose) {
    // First the peer, from the client certificate (SEC-02, REVIEW-5ae0ef1): the handshake id and
    // purpose rules below are for several contexts "between a pair of SEPPs", so they apply only
    // among the contexts of the peer the certificate names -- never to pick between peers.
    std::vector<std::string> names;
    for (const auto& n : cert_dns_names) {
        names.push_back(lower(n));
    }
    std::vector<Context> peer_contexts;
    for (const auto& c : contexts) {
        if (contains(names, c.peer_fqdn)) {
            peer_contexts.push_back(c);
        }
    }
    // A certificate naming several SEPPs (one common certificate): the Via header decides (the
    // clause's NOTE).
    const auto sepp_names = std::ranges::count_if(
        names, [](const std::string& n) { return network_domain_of_host(n).has_value(); });
    if (sepp_names > 1) {
        const auto via_names = via_sepps(via);
        std::erase_if(peer_contexts,
                      [&](const Context& c) { return !contains(via_names, c.peer_fqdn); });
    }
    if (peer_contexts.empty()) {
        return std::nullopt;
    }
    // Still more than one peer (e.g. a Via naming several): the request identifies none of them.
    if (std::ranges::any_of(peer_contexts, [&](const Context& c) {
            return c.peer_fqdn != peer_contexts.front().peer_fqdn;
        })) {
        return std::nullopt;
    }
    if (peer_contexts.size() == 1) {
        return peer_contexts.front();
    }
    // Several N32-c contexts with this one peer.
    const auto& own = peer_contexts;
    const bool ids_exchanged =
        std::ranges::all_of(own, [](const Context& c) { return c.handshake_id.has_value(); });
    if (ids_exchanged) {
        if (!handshake_id) {
            return std::nullopt;
        }
        const auto it = std::ranges::find_if(
            own, [&](const Context& c) { return lower(*c.handshake_id) == lower(*handshake_id); });
        return it == own.end() ? std::nullopt : std::optional{*it};
    }
    const std::string p = purpose.value_or(sbi_gen::N32Purpose::ROAMING);
    std::optional<Context> match;
    for (const auto& c : own) {
        if (contains(c.purposes, p)) {
            if (match) {
                return std::nullopt; // ambiguous
            }
            match = c;
        }
    }
    return match;
}

void PrinsStore::put(PrinsContext c) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(contexts_, [&](const PrinsContext& x) { return x.peer_fqdn == c.peer_fqdn; });
    contexts_.push_back(std::move(c));
}

std::optional<PrinsContext> PrinsStore::by_peer(const std::string& peer_fqdn) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::ranges::find_if(
        contexts_, [&](const PrinsContext& x) { return x.peer_fqdn == peer_fqdn; });
    return it == contexts_.end() ? std::nullopt : std::optional{*it};
}

std::optional<PrinsContext> PrinsStore::by_local_id(const std::string& id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::ranges::find_if(
        contexts_, [&](const PrinsContext& x) { return lower(x.local_id) == lower(id); });
    return it == contexts_.end() ? std::nullopt : std::optional{*it};
}

bool PrinsStore::erase_local_id(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::erase_if(contexts_,
                         [&](const PrinsContext& x) { return lower(x.local_id) == lower(id); }) > 0;
}

std::vector<PrinsContext> PrinsStore::all() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return contexts_;
}

void PrinsStore::set_outgoing(const std::string& peer_fqdn, bool in_progress) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (in_progress) {
        outgoing_.insert(peer_fqdn);
    } else {
        outgoing_.erase(peer_fqdn);
    }
}

bool PrinsStore::outgoing(const std::string& peer_fqdn) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return outgoing_.contains(peer_fqdn);
}

std::string random_n32f_context_id() {
    std::uint8_t buf[8];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        throw std::runtime_error("sepp: RAND_bytes failed");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const auto b : buf) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
    }
    return out;
}

std::vector<std::uint8_t>
hkdf_expand(const std::vector<std::uint8_t>& prk, const std::string& info, std::size_t length) {
    std::vector<std::uint8_t> out(length);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    const bool ok =
        ctx != nullptr && EVP_PKEY_derive_init(ctx) == 1 &&
        EVP_PKEY_CTX_set_hkdf_mode(ctx, EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) == 1 &&
        EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_key(ctx, prk.data(), static_cast<int>(prk.size())) == 1 &&
        EVP_PKEY_CTX_add1_hkdf_info(ctx,
                                    reinterpret_cast<const unsigned char*>(info.data()),
                                    static_cast<int>(info.size())) == 1 &&
        EVP_PKEY_derive(ctx, out.data(), &length) == 1;
    EVP_PKEY_CTX_free(ctx);
    if (!ok) {
        throw std::runtime_error("sepp: HKDF-Expand failed");
    }
    return out;
}

std::vector<std::uint8_t> n32_kdf(const std::vector<std::uint8_t>& master_key,
                                  const std::string& n32f_context_id,
                                  const std::string& label,
                                  std::size_t length) {
    return hkdf_expand(master_key, "N32" + n32f_context_id + label, length);
}

std::string key_fingerprint(const std::vector<std::uint8_t>& key) {
    std::vector<std::uint8_t> in{'f', 'p'};
    in.insert(in.end(), key.begin(), key.end());
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(in.data(), in.size(), md, &len, EVP_sha256(), nullptr);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < 4 && i < len; ++i) {
        out += kHex[md[i] >> 4];
        out += kHex[md[i] & 0x0F];
    }
    return out;
}

ParamDecision exchange_params(const NegotiationPolicy& policy,
                              const std::optional<Context>& capability,
                              const std::optional<PrinsContext>& existing,
                              const sbi_gen::SecParamExchReqData& request,
                              const std::optional<std::vector<std::uint8_t>>& master_key,
                              bool outgoing_in_progress) {
    ParamDecision d;
    const auto fail = [&](int status, std::string cause, std::string detail) {
        d.status = status;
        d.cause = std::move(cause);
        d.detail = std::move(detail);
        return d;
    };
    if (!capability || capability->security != sbi_gen::SecurityCapability::PRINS) {
        return fail(403,
                    "NEGOTIATION_NOT_ALLOWED",
                    "PRINS was not selected in a security capability negotiation with this SEPP");
    }
    if (!is_handshake_id(request.n32fContextId)) { // same 16-hex-digit pattern
        return fail(400, "MANDATORY_IE_INCORRECT", "n32fContextId must be 16 hexadecimal digits");
    }
    sbi_gen::SecParamExchRspData rsp;
    rsp.sender = policy.own_fqdn;

    if (request.jweCipherSuiteList) {
        // 5.2.3.2 collision: the lexicographically preceding FQDN continues its own procedure.
        if (outgoing_in_progress && lower(policy.own_fqdn) < capability->peer_fqdn) {
            return fail(409,
                        "SECURITY_PARAM_EXCHANGE_COLLISION",
                        "this SEPP's own parameter exchange takes precedence");
        }
        std::optional<std::string> jwe;
        for (const auto& c : policy.jwe_cipher_suites) {
            if (contains(*request.jweCipherSuiteList, c)) {
                jwe = c;
                break;
            }
        }
        if (!jwe) {
            return fail(403, "NEGOTIATION_NOT_ALLOWED", "no JWE cipher suite in common");
        }
        if (!master_key || master_key->size() != kN32MasterKeyLength) {
            return fail(500, "SYSTEM_FAILURE", "no TLS exporter output for this N32-c connection");
        }
        PrinsContext ctx;
        ctx.peer_fqdn = capability->peer_fqdn;
        ctx.local_id = random_n32f_context_id();
        ctx.peer_id = lower(request.n32fContextId);
        ctx.initiator = false;
        ctx.jwe_cipher = *jwe;
        if (request.jwsCipherSuiteList) {
            for (const auto& c : policy.jws_cipher_suites) {
                if (contains(*request.jwsCipherSuiteList, c)) {
                    ctx.jws_cipher = c;
                    break;
                }
            }
        }
        ctx.master_key = *master_key;
        rsp.n32fContextId = ctx.local_id;
        rsp.selectedJweCipherSuite = ctx.jwe_cipher;
        rsp.selectedJwsCipherSuite = ctx.jws_cipher;
        d.response = rsp;
        d.context = std::move(ctx);
        return d;
    }
    if (request.protectionPolicyInfo) {
        // 5.2.3.3: after the cipher suite negotiation, on the N32-f context it created.
        if (!existing || lower(existing->peer_id) != lower(request.n32fContextId)) {
            return fail(403,
                        "NEGOTIATION_NOT_ALLOWED",
                        "no cipher suite has been negotiated for this n32fContextId");
        }
        PrinsContext ctx = *existing;
        ctx.peer_policy = *request.protectionPolicyInfo;
        ctx.selected_policy = policy.protection_policy;
        rsp.n32fContextId = ctx.local_id;
        rsp.selProtectionPolicyInfo = policy.protection_policy;
        d.response = rsp;
        d.context = std::move(ctx);
        return d;
    }
    return fail(400,
                "MANDATORY_IE_MISSING",
                "neither jweCipherSuiteList nor protectionPolicyInfo (security profiles and RI "
                "security information are not supported)");
}

std::string telescopic_label(const std::string& foreign_fqdn) {
    const std::string in = lower(foreign_fqdn);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(in.data(), in.size(), md, &len, EVP_sha256(), nullptr);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "tel-";
    for (unsigned int i = 0; i < 8 && i < len; ++i) {
        out += kHex[md[i] >> 4];
        out += kHex[md[i] & 0x0F];
    }
    return out;
}

std::string TelescopicTable::label_for(const std::string& foreign_fqdn) {
    const std::string label = telescopic_label(foreign_fqdn);
    const std::lock_guard<std::mutex> lock(mutex_);
    by_label_[label] = lower(foreign_fqdn);
    return label;
}

std::optional<std::string> TelescopicTable::foreign_for(const std::string& label) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_label_.find(lower(label));
    return it == by_label_.end() ? std::nullopt : std::optional{it->second};
}

std::optional<std::string> telescopic_label_of_host(std::string_view host,
                                                    const std::string& sepp_fqdn) {
    const std::string h = lower(host);
    const std::string suffix = "." + lower(sepp_fqdn);
    if (h.size() <= suffix.size() ||
        h.compare(h.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::nullopt;
    }
    std::string label = h.substr(0, h.size() - suffix.size());
    if (label.find('.') != std::string::npos) {
        return std::nullopt;
    }
    return label;
}

} // namespace sepp
