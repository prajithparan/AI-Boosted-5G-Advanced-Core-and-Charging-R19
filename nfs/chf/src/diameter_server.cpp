#include "diameter_server.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <spdlog/spdlog.h>

#include "diameter_core/avp.hpp"
#include "diameter_core/dictionary.hpp"
#include "diameter_core/header.hpp"

namespace chf {

namespace {

using namespace diameter_core;

// This project's own lab-internal Diameter identity -- disclosed, project-internal convention
// (no real IANA-assigned enterprise number, no real registered DNS realm), matching the same
// per-NF-name convention already used for TLS cert CNs (scripts/gen-lab-pki.sh's `-subj
// "/O=5gc-r19 Lab/CN=${nf}"`).
constexpr std::uint32_t kNoVendorId = 0;

std::vector<std::uint8_t> build_cea(const Header& request_header,
                                    const std::string& origin_host,
                                    const std::string& origin_realm,
                                    std::int32_t result_code) {
    std::vector<std::uint8_t> avps_bytes;

    Avp result_code_avp;
    result_code_avp.code = dictionary::Avp::kResultCode;
    result_code_avp.flags = AvpFlag::kMandatory;
    result_code_avp.data = encode_integer32(result_code);
    encode_avp(avps_bytes, result_code_avp);

    Avp origin_host_avp;
    origin_host_avp.code = dictionary::Avp::kOriginHost;
    origin_host_avp.flags = AvpFlag::kMandatory;
    origin_host_avp.data = encode_octet_string(origin_host);
    encode_avp(avps_bytes, origin_host_avp);

    Avp origin_realm_avp;
    origin_realm_avp.code = dictionary::Avp::kOriginRealm;
    origin_realm_avp.flags = AvpFlag::kMandatory;
    origin_realm_avp.data = encode_octet_string(origin_realm);
    encode_avp(avps_bytes, origin_realm_avp);

    // Host-IP-Address is real-spec-REQUIRED (1*) in a real CEA -- this project's own lab
    // convention is loopback-only (every NF binds 0.0.0.0/connects via 127.0.0.1, e.g. CHF's own
    // HTTP/2 listener), so 127.0.0.1 is sent, not a placeholder.
    Avp host_ip_avp;
    host_ip_avp.code = dictionary::Avp::kHostIpAddress;
    host_ip_avp.flags = AvpFlag::kMandatory;
    host_ip_avp.data = encode_address_ipv4((127u << 24) | 1u);
    encode_avp(avps_bytes, host_ip_avp);

    Avp vendor_id_avp;
    vendor_id_avp.code = dictionary::Avp::kVendorId;
    vendor_id_avp.flags = AvpFlag::kMandatory;
    vendor_id_avp.data = encode_unsigned32(kNoVendorId);
    encode_avp(avps_bytes, vendor_id_avp);

    Avp product_name_avp;
    product_name_avp.code = dictionary::Avp::kProductName;
    product_name_avp.flags =
        0; // real spec: Product-Name is NOT flagged Mandatory (dict_base_proto.c)
    product_name_avp.data = encode_octet_string("5gc-r19-chf");
    encode_avp(avps_bytes, product_name_avp);

    // Auth-Application-Id=4: real RFC 4006 Diameter Credit-Control Application ID (real Gy CCR/
    // CCA, ADR-0059 Stage 3).
    Avp auth_app_id_avp;
    auth_app_id_avp.code = dictionary::Avp::kAuthApplicationId;
    auth_app_id_avp.flags = AvpFlag::kMandatory;
    auth_app_id_avp.data = encode_unsigned32(dictionary::Dcc::kApplicationId);
    encode_avp(avps_bytes, auth_app_id_avp);

    // Acct-Application-Id=3: real RFC 6733 "Diameter Base Accounting" application ID (real Rf
    // ACR/ACA, ADR-0059 Stage 4) -- a real, distinct capability from Auth-Application-Id above,
    // both genuinely advertised since this CHF process accepts both.
    Avp acct_app_id_avp;
    acct_app_id_avp.code = dictionary::Avp::kAcctApplicationId;
    acct_app_id_avp.flags = AvpFlag::kMandatory;
    acct_app_id_avp.data = encode_unsigned32(dictionary::BaseAccounting::kApplicationId);
    encode_avp(avps_bytes, acct_app_id_avp);

    // TS 29.219 §5.1.5 (real Sy, ADR-0059 Stage 4 Sy half): a real vendor-specific application is
    // advertised differently from Gy/Rf's plain Auth-/Acct-Application-Id above -- Supported-
    // Vendor-Id (3GPP=10415) at the top level, plus a Vendor-Specific-Application-Id grouped AVP
    // ({ Vendor-Id=10415 } [ Auth-Application-Id=16777302 ]), exactly as the real spec text
    // requires.
    Avp supported_vendor_id_avp;
    supported_vendor_id_avp.code = dictionary::Avp::kSupportedVendorId;
    supported_vendor_id_avp.flags = AvpFlag::kMandatory;
    supported_vendor_id_avp.data = encode_unsigned32(dictionary::Sy::kVendorId);
    encode_avp(avps_bytes, supported_vendor_id_avp);

    std::vector<std::uint8_t> vsai_bytes;
    Avp vsai_vendor_id_avp;
    vsai_vendor_id_avp.code = dictionary::Avp::kVendorId;
    vsai_vendor_id_avp.flags = AvpFlag::kMandatory;
    vsai_vendor_id_avp.data = encode_unsigned32(dictionary::Sy::kVendorId);
    encode_avp(vsai_bytes, vsai_vendor_id_avp);
    Avp vsai_auth_app_id_avp;
    vsai_auth_app_id_avp.code = dictionary::Avp::kAuthApplicationId;
    vsai_auth_app_id_avp.flags = AvpFlag::kMandatory;
    vsai_auth_app_id_avp.data = encode_unsigned32(dictionary::Sy::kApplicationId);
    encode_avp(vsai_bytes, vsai_auth_app_id_avp);
    Avp vsai_avp;
    vsai_avp.code = dictionary::Avp::kVendorSpecificApplicationId;
    vsai_avp.flags = AvpFlag::kMandatory;
    vsai_avp.data = vsai_bytes;
    encode_avp(avps_bytes, vsai_avp);

    Header answer_header;
    answer_header.flags = 0; // R bit cleared: this is an Answer, not a Request
    answer_header.command_code = dictionary::Command::kCapabilitiesExchange;
    answer_header.application_id = 0; // CER/CEA itself always uses Application-Id 0
    answer_header.hop_by_hop_id = request_header.hop_by_hop_id; // real spec: echoed from request
    answer_header.end_to_end_id = request_header.end_to_end_id; // real spec: echoed from request

    auto message = encode_header(answer_header, static_cast<std::uint32_t>(avps_bytes.size()));
    message.insert(message.end(), avps_bytes.begin(), avps_bytes.end());
    return message;
}

// P4.5/ADR-0060 Stage 3: real CCR decode (RFC 4006). Only the fields this project's charging
// engine (charging_engine.hpp) actually consumes are extracted -- Session-Id, CC-Request-Type,
// CC-Request-Number (all mandatory per dict_dcca.c:1360-1367), an optional Subscription-Id (0+,
// dict_dcca.c:1374 RULE_OPTIONAL) used to build a SUPI when its Subscription-Id-Type is
// END_USER_IMSI, and 0+ Multiple-Services-Credit-Control groups, each carrying a Rating-Group and
// an optional Used-Service-Unit -> CC-Total-Octets (the "volume already consumed" figure
// TS 32.291's own usedUnitContainer[].totalVolume carries on the 5G/HTTP side).
struct DecodedMscc {
    std::uint32_t rating_group = 0;
    std::optional<std::uint64_t> used_total_octets;
};

struct DecodedCcr {
    std::string session_id;
    std::int32_t cc_request_type = 0;
    std::uint32_t cc_request_number = 0;
    std::string supi; // "" if no Subscription-Id (or none of Subscription-Id-Type END_USER_IMSI)
    std::vector<DecodedMscc> mscc;
};

std::optional<DecodedCcr> decode_ccr(const std::vector<Avp>& avps) {
    const auto* session_id_avp = find_avp(avps, dictionary::Avp::kSessionId);
    const auto* cc_request_type_avp = find_avp(avps, dictionary::Dcc::kCcRequestType);
    const auto* cc_request_number_avp = find_avp(avps, dictionary::Dcc::kCcRequestNumber);
    if (session_id_avp == nullptr || cc_request_type_avp == nullptr ||
        cc_request_number_avp == nullptr) {
        return std::nullopt;
    }
    const auto session_id = decode_octet_string(session_id_avp->data);
    const auto cc_request_type = decode_integer32(cc_request_type_avp->data);
    const auto cc_request_number = decode_unsigned32(cc_request_number_avp->data);
    if (!session_id.has_value() || !cc_request_type.has_value() || !cc_request_number.has_value()) {
        return std::nullopt;
    }

    DecodedCcr out;
    out.session_id = *session_id;
    out.cc_request_type = *cc_request_type;
    out.cc_request_number = *cc_request_number;

    // Subscription-Id (Grouped, 0+ per dict_dcca.c:1374) -- first END_USER_IMSI instance wins,
    // mapped onto this project's own real "imsi-<digits>" SUPI convention (already used
    // throughout, e.g. nfs/udm/src/main.cpp's seeded subscribers).
    for (const auto& avp : avps) {
        if (avp.code != dictionary::Dcc::kSubscriptionId) {
            continue;
        }
        const auto sub_avps = decode_avps(avp.data);
        if (!sub_avps.has_value()) {
            continue;
        }
        const auto* type_avp = find_avp(*sub_avps, dictionary::Dcc::kSubscriptionIdType);
        const auto* data_avp = find_avp(*sub_avps, dictionary::Dcc::kSubscriptionIdData);
        if (type_avp == nullptr || data_avp == nullptr) {
            continue;
        }
        const auto type = decode_integer32(type_avp->data);
        const auto data = decode_octet_string(data_avp->data);
        if (!type.has_value() || !data.has_value()) {
            continue;
        }
        if (*type == dictionary::Dcc::SubscriptionIdType::kEndUserImsi) {
            out.supi = "imsi-" + *data;
            break;
        }
    }

    // Multiple-Services-Credit-Control (Grouped, 0+ per dict_dcca.c:1227/1374).
    for (const auto& avp : avps) {
        if (avp.code != dictionary::Dcc::kMultipleServicesCreditControl) {
            continue;
        }
        const auto mscc_avps = decode_avps(avp.data);
        if (!mscc_avps.has_value()) {
            continue;
        }
        DecodedMscc mscc;
        if (const auto* rg_avp = find_avp(*mscc_avps, dictionary::Dcc::kRatingGroup);
            rg_avp != nullptr) {
            if (const auto rg = decode_unsigned32(rg_avp->data); rg.has_value()) {
                mscc.rating_group = *rg;
            }
        }
        if (const auto* usu_avp = find_avp(*mscc_avps, dictionary::Dcc::kUsedServiceUnit);
            usu_avp != nullptr) {
            const auto usu_avps = decode_avps(usu_avp->data);
            if (usu_avps.has_value()) {
                if (const auto* octets_avp = find_avp(*usu_avps, dictionary::Dcc::kCcTotalOctets);
                    octets_avp != nullptr) {
                    mscc.used_total_octets = decode_unsigned64(octets_avp->data);
                }
            }
        }
        out.mscc.push_back(mscc);
    }

    return out;
}

// P4.5/ADR-0060 Stage 3: real CCA encode. `granted_per_group` carries one (Rating-Group, optional
// GrantedUnit) pair per input Multiple-Services-Credit-Control -- a GrantedUnit only becomes a
// real Granted-Service-Unit AVP when charge_one_usage actually granted+reserved something (empty
// otherwise, mirroring the HTTP path's own "grantedUnit omitted when nothing was rated/reserved"
// shape, MultipleUnitInformation in main.cpp's Create/Update handlers).
std::vector<std::uint8_t>
build_cca(const Header& request_header,
          const std::string& origin_host,
          const std::string& origin_realm,
          const std::string& session_id,
          std::int32_t cc_request_type,
          std::uint32_t cc_request_number,
          std::int32_t result_code,
          const std::vector<std::pair<std::uint32_t, std::optional<sbi_gen::GrantedUnit>>>&
              granted_per_group) {
    std::vector<std::uint8_t> avps_bytes;

    Avp session_id_avp;
    session_id_avp.code = dictionary::Avp::kSessionId;
    session_id_avp.flags = AvpFlag::kMandatory;
    session_id_avp.data = encode_octet_string(session_id);
    encode_avp(avps_bytes, session_id_avp);

    Avp result_code_avp;
    result_code_avp.code = dictionary::Avp::kResultCode;
    result_code_avp.flags = AvpFlag::kMandatory;
    result_code_avp.data = encode_integer32(result_code);
    encode_avp(avps_bytes, result_code_avp);

    Avp origin_host_avp;
    origin_host_avp.code = dictionary::Avp::kOriginHost;
    origin_host_avp.flags = AvpFlag::kMandatory;
    origin_host_avp.data = encode_octet_string(origin_host);
    encode_avp(avps_bytes, origin_host_avp);

    Avp origin_realm_avp;
    origin_realm_avp.code = dictionary::Avp::kOriginRealm;
    origin_realm_avp.flags = AvpFlag::kMandatory;
    origin_realm_avp.data = encode_octet_string(origin_realm);
    encode_avp(avps_bytes, origin_realm_avp);

    Avp cc_request_type_avp;
    cc_request_type_avp.code = dictionary::Dcc::kCcRequestType;
    cc_request_type_avp.flags = AvpFlag::kMandatory;
    cc_request_type_avp.data = encode_integer32(cc_request_type);
    encode_avp(avps_bytes, cc_request_type_avp);

    Avp cc_request_number_avp;
    cc_request_number_avp.code = dictionary::Dcc::kCcRequestNumber;
    cc_request_number_avp.flags = AvpFlag::kMandatory;
    cc_request_number_avp.data = encode_unsigned32(cc_request_number);
    encode_avp(avps_bytes, cc_request_number_avp);

    for (const auto& [rating_group, grant] : granted_per_group) {
        std::vector<std::uint8_t> mscc_bytes;

        Avp rg_avp;
        rg_avp.code = dictionary::Dcc::kRatingGroup;
        rg_avp.flags = AvpFlag::kMandatory;
        rg_avp.data = encode_unsigned32(rating_group);
        encode_avp(mscc_bytes, rg_avp);

        if (grant.has_value()) {
            std::vector<std::uint8_t> gsu_bytes;
            // Same real, narrow unit-conversion choice charging_engine.cpp's build_rating_grant
            // already made: a totalVolume grant maps to CC-Total-Octets, otherwise
            // serviceSpecificUnits maps to CC-Service-Specific-Units.
            if (grant->totalVolume.has_value()) {
                Avp octets_avp;
                octets_avp.code = dictionary::Dcc::kCcTotalOctets;
                octets_avp.flags = AvpFlag::kMandatory;
                octets_avp.data =
                    encode_unsigned64(static_cast<std::uint64_t>(*grant->totalVolume));
                encode_avp(gsu_bytes, octets_avp);
            } else if (grant->serviceSpecificUnits.has_value()) {
                Avp units_avp;
                units_avp.code = dictionary::Dcc::kCcServiceSpecificUnits;
                units_avp.flags = AvpFlag::kMandatory;
                units_avp.data =
                    encode_unsigned64(static_cast<std::uint64_t>(*grant->serviceSpecificUnits));
                encode_avp(gsu_bytes, units_avp);
            }
            if (!gsu_bytes.empty()) {
                Avp gsu_avp;
                gsu_avp.code = dictionary::Dcc::kGrantedServiceUnit;
                gsu_avp.flags = AvpFlag::kMandatory;
                gsu_avp.data = gsu_bytes;
                encode_avp(mscc_bytes, gsu_avp);
            }
        }

        Avp mscc_avp;
        mscc_avp.code = dictionary::Dcc::kMultipleServicesCreditControl;
        mscc_avp.flags = AvpFlag::kMandatory;
        mscc_avp.data = mscc_bytes;
        encode_avp(avps_bytes, mscc_avp);
    }

    Header answer_header;
    answer_header.flags = 0; // R bit cleared: this is an Answer, not a Request
    answer_header.command_code = dictionary::Command::kCreditControl;
    answer_header.application_id = request_header.application_id;
    answer_header.hop_by_hop_id = request_header.hop_by_hop_id; // real spec: echoed from request
    answer_header.end_to_end_id = request_header.end_to_end_id; // real spec: echoed from request

    auto message = encode_header(answer_header, static_cast<std::uint32_t>(avps_bytes.size()));
    message.insert(message.end(), avps_bytes.begin(), avps_bytes.end());
    return message;
}

// P4.5/ADR-0059 Stage 4 (Rf half): real ACR decode (RFC 6733 §9.3, dict_base_proto.c:3212-3230).
// Only Session-Id, Accounting-Record-Type, Accounting-Record-Number are extracted -- the mandatory
// fields this project's `Nchf_OfflineOnlyCharging` normalize path actually consumes (no rating
// engine, no per-usage AVPs the way Gy's own MSCC carries -- `Nchf_OfflineOnlyCharging`'s real
// schema has no `multipleUnitInformation`/grant field at all, see main.cpp's own header).
struct DecodedAcr {
    std::string session_id;
    std::int32_t accounting_record_type = 0;
    std::uint32_t accounting_record_number = 0;
};

std::optional<DecodedAcr> decode_acr(const std::vector<Avp>& avps) {
    const auto* session_id_avp = find_avp(avps, dictionary::Avp::kSessionId);
    const auto* record_type_avp = find_avp(avps, dictionary::Avp::kAccountingRecordType);
    const auto* record_number_avp = find_avp(avps, dictionary::Avp::kAccountingRecordNumber);
    if (session_id_avp == nullptr || record_type_avp == nullptr || record_number_avp == nullptr) {
        return std::nullopt;
    }
    const auto session_id = decode_octet_string(session_id_avp->data);
    const auto record_type = decode_integer32(record_type_avp->data);
    const auto record_number = decode_unsigned32(record_number_avp->data);
    if (!session_id.has_value() || !record_type.has_value() || !record_number.has_value()) {
        return std::nullopt;
    }
    return DecodedAcr{*session_id, *record_type, *record_number};
}

// P4.5/ADR-0059 Stage 4 (Rf half): real ACA encode (RFC 6733 §9.3, dict_base_proto.c:3291-3316).
std::vector<std::uint8_t> build_aca(const Header& request_header,
                                    const std::string& origin_host,
                                    const std::string& origin_realm,
                                    const std::string& session_id,
                                    std::int32_t accounting_record_type,
                                    std::uint32_t accounting_record_number,
                                    std::int32_t result_code) {
    std::vector<std::uint8_t> avps_bytes;

    Avp session_id_avp;
    session_id_avp.code = dictionary::Avp::kSessionId;
    session_id_avp.flags = AvpFlag::kMandatory;
    session_id_avp.data = encode_octet_string(session_id);
    encode_avp(avps_bytes, session_id_avp);

    Avp result_code_avp;
    result_code_avp.code = dictionary::Avp::kResultCode;
    result_code_avp.flags = AvpFlag::kMandatory;
    result_code_avp.data = encode_integer32(result_code);
    encode_avp(avps_bytes, result_code_avp);

    Avp origin_host_avp;
    origin_host_avp.code = dictionary::Avp::kOriginHost;
    origin_host_avp.flags = AvpFlag::kMandatory;
    origin_host_avp.data = encode_octet_string(origin_host);
    encode_avp(avps_bytes, origin_host_avp);

    Avp origin_realm_avp;
    origin_realm_avp.code = dictionary::Avp::kOriginRealm;
    origin_realm_avp.flags = AvpFlag::kMandatory;
    origin_realm_avp.data = encode_octet_string(origin_realm);
    encode_avp(avps_bytes, origin_realm_avp);

    Avp record_type_avp;
    record_type_avp.code = dictionary::Avp::kAccountingRecordType;
    record_type_avp.flags = AvpFlag::kMandatory;
    record_type_avp.data = encode_integer32(accounting_record_type);
    encode_avp(avps_bytes, record_type_avp);

    Avp record_number_avp;
    record_number_avp.code = dictionary::Avp::kAccountingRecordNumber;
    record_number_avp.flags = AvpFlag::kMandatory;
    record_number_avp.data = encode_unsigned32(accounting_record_number);
    encode_avp(avps_bytes, record_number_avp);

    Header answer_header;
    answer_header.flags = 0; // R bit cleared: this is an Answer, not a Request
    answer_header.command_code = dictionary::Command::kAccounting;
    answer_header.application_id = request_header.application_id;
    answer_header.hop_by_hop_id = request_header.hop_by_hop_id; // real spec: echoed from request
    answer_header.end_to_end_id = request_header.end_to_end_id; // real spec: echoed from request

    auto message = encode_header(answer_header, static_cast<std::uint32_t>(avps_bytes.size()));
    message.insert(message.end(), avps_bytes.begin(), avps_bytes.end());
    return message;
}

// P4.5/ADR-0059 Stage 4 (Sy half): real SLR decode (TS 29.219 §5.6.2). Session-Id and SL-Request-
// Type are mandatory; Subscription-Id (reused from RFC 4006, 0+, same decode as CCR's own) and
// Policy-Counter-Identifier (0+, real scalar UTF8String -- unlike Gy's own grouped MSCC) are both
// real-spec-optional.
struct DecodedSlr {
    std::string session_id;
    std::int32_t sl_request_type = 0;
    std::string supi; // "" if no Subscription-Id (or none of Subscription-Id-Type END_USER_IMSI)
    std::vector<std::string> policy_counter_ids;
};

std::optional<DecodedSlr> decode_slr(const std::vector<Avp>& avps) {
    const auto* session_id_avp = find_avp(avps, dictionary::Avp::kSessionId);
    const auto* sl_request_type_avp = find_avp(avps, dictionary::Sy::kSlRequestType);
    if (session_id_avp == nullptr || sl_request_type_avp == nullptr) {
        return std::nullopt;
    }
    const auto session_id = decode_octet_string(session_id_avp->data);
    const auto sl_request_type = decode_integer32(sl_request_type_avp->data);
    if (!session_id.has_value() || !sl_request_type.has_value()) {
        return std::nullopt;
    }

    DecodedSlr out;
    out.session_id = *session_id;
    out.sl_request_type = *sl_request_type;

    // Subscription-Id (Grouped, RFC 4006 AVP reused by TS 29.219 §5.4's own "Sy re-used AVPs"
    // table) -- same real END_USER_IMSI-wins decode as decode_ccr's own.
    for (const auto& avp : avps) {
        if (avp.code != dictionary::Dcc::kSubscriptionId) {
            continue;
        }
        const auto sub_avps = decode_avps(avp.data);
        if (!sub_avps.has_value()) {
            continue;
        }
        const auto* type_avp = find_avp(*sub_avps, dictionary::Dcc::kSubscriptionIdType);
        const auto* data_avp = find_avp(*sub_avps, dictionary::Dcc::kSubscriptionIdData);
        if (type_avp == nullptr || data_avp == nullptr) {
            continue;
        }
        const auto type = decode_integer32(type_avp->data);
        const auto data = decode_octet_string(data_avp->data);
        if (!type.has_value() || !data.has_value()) {
            continue;
        }
        if (*type == dictionary::Dcc::SubscriptionIdType::kEndUserImsi) {
            out.supi = "imsi-" + *data;
            break;
        }
    }

    for (const auto& avp : avps) {
        if (avp.code == dictionary::Sy::kPolicyCounterIdentifier) {
            if (const auto id = decode_octet_string(avp.data); id.has_value()) {
                out.policy_counter_ids.push_back(*id);
            }
        }
    }

    return out;
}

// P4.5/ADR-0059 Stage 4 (Sy half): real SLA encode (TS 29.219 §5.6.3). `policy_counter_status`
// carries one (Policy-Counter-Identifier, Policy-Counter-Status) pair per real Policy-Counter-
// Status-Report AVP -- built from the exact same `build_spending_limit_status` function main.cpp's
// HTTP Subscribe/Update handlers already call (the single-code-path property for Sy, same as
// Gy/Rf's own shared-function calls).
std::vector<std::uint8_t>
build_sla(const Header& request_header,
          const std::string& origin_host,
          const std::string& origin_realm,
          const std::string& session_id,
          std::int32_t result_code,
          const std::vector<std::pair<std::string, std::string>>& policy_counter_status) {
    std::vector<std::uint8_t> avps_bytes;

    Avp session_id_avp;
    session_id_avp.code = dictionary::Avp::kSessionId;
    session_id_avp.flags = AvpFlag::kMandatory;
    session_id_avp.data = encode_octet_string(session_id);
    encode_avp(avps_bytes, session_id_avp);

    Avp result_code_avp;
    result_code_avp.code = dictionary::Avp::kResultCode;
    result_code_avp.flags = AvpFlag::kMandatory;
    result_code_avp.data = encode_integer32(result_code);
    encode_avp(avps_bytes, result_code_avp);

    Avp origin_host_avp;
    origin_host_avp.code = dictionary::Avp::kOriginHost;
    origin_host_avp.flags = AvpFlag::kMandatory;
    origin_host_avp.data = encode_octet_string(origin_host);
    encode_avp(avps_bytes, origin_host_avp);

    Avp origin_realm_avp;
    origin_realm_avp.code = dictionary::Avp::kOriginRealm;
    origin_realm_avp.flags = AvpFlag::kMandatory;
    origin_realm_avp.data = encode_octet_string(origin_realm);
    encode_avp(avps_bytes, origin_realm_avp);

    Avp auth_app_id_avp;
    auth_app_id_avp.code = dictionary::Avp::kAuthApplicationId;
    auth_app_id_avp.flags = AvpFlag::kMandatory;
    auth_app_id_avp.data = encode_unsigned32(dictionary::Sy::kApplicationId);
    encode_avp(avps_bytes, auth_app_id_avp);

    for (const auto& [counter_id, status] : policy_counter_status) {
        std::vector<std::uint8_t> report_bytes;

        Avp id_avp;
        id_avp.code = dictionary::Sy::kPolicyCounterIdentifier;
        id_avp.flags = AvpFlag::kVendor | AvpFlag::kMandatory;
        id_avp.vendor_id = dictionary::Sy::kVendorId;
        id_avp.data = encode_octet_string(counter_id);
        encode_avp(report_bytes, id_avp);

        Avp status_avp;
        status_avp.code = dictionary::Sy::kPolicyCounterStatus;
        status_avp.flags = AvpFlag::kVendor | AvpFlag::kMandatory;
        status_avp.vendor_id = dictionary::Sy::kVendorId;
        status_avp.data = encode_octet_string(status);
        encode_avp(report_bytes, status_avp);

        Avp report_avp;
        report_avp.code = dictionary::Sy::kPolicyCounterStatusReport;
        report_avp.flags = AvpFlag::kVendor | AvpFlag::kMandatory;
        report_avp.vendor_id = dictionary::Sy::kVendorId;
        report_avp.data = report_bytes;
        encode_avp(avps_bytes, report_avp);
    }

    Header answer_header;
    answer_header.flags = 0; // R bit cleared: this is an Answer, not a Request
    answer_header.command_code = dictionary::Command::kSpendingLimit;
    answer_header.application_id = request_header.application_id;
    answer_header.hop_by_hop_id = request_header.hop_by_hop_id; // real spec: echoed from request
    answer_header.end_to_end_id = request_header.end_to_end_id; // real spec: echoed from request

    auto message = encode_header(answer_header, static_cast<std::uint32_t>(avps_bytes.size()));
    message.insert(message.end(), avps_bytes.begin(), avps_bytes.end());
    return message;
}

// P4.5/ADR-0059 Stage 4 (Sy half): real STR decode (RFC 6733, reused per TS 29.219 §4.5.3.1 for
// the Final Spending Limit Report Request -- Sy's own real Unsubscribe trigger).
struct DecodedStr {
    std::string session_id;
    std::int32_t termination_cause = 0;
};

std::optional<DecodedStr> decode_str(const std::vector<Avp>& avps) {
    const auto* session_id_avp = find_avp(avps, dictionary::Avp::kSessionId);
    const auto* cause_avp = find_avp(avps, dictionary::Avp::kTerminationCause);
    if (session_id_avp == nullptr || cause_avp == nullptr) {
        return std::nullopt;
    }
    const auto session_id = decode_octet_string(session_id_avp->data);
    const auto cause = decode_integer32(cause_avp->data);
    if (!session_id.has_value() || !cause.has_value()) {
        return std::nullopt;
    }
    return DecodedStr{*session_id, *cause};
}

// P4.5/ADR-0059 Stage 4 (Sy half): real STA encode (RFC 6733 §8.5.2, dict_base_proto.c:3040-3060).
std::vector<std::uint8_t> build_sta(const Header& request_header,
                                    const std::string& origin_host,
                                    const std::string& origin_realm,
                                    const std::string& session_id,
                                    std::int32_t result_code) {
    std::vector<std::uint8_t> avps_bytes;

    Avp session_id_avp;
    session_id_avp.code = dictionary::Avp::kSessionId;
    session_id_avp.flags = AvpFlag::kMandatory;
    session_id_avp.data = encode_octet_string(session_id);
    encode_avp(avps_bytes, session_id_avp);

    Avp result_code_avp;
    result_code_avp.code = dictionary::Avp::kResultCode;
    result_code_avp.flags = AvpFlag::kMandatory;
    result_code_avp.data = encode_integer32(result_code);
    encode_avp(avps_bytes, result_code_avp);

    Avp origin_host_avp;
    origin_host_avp.code = dictionary::Avp::kOriginHost;
    origin_host_avp.flags = AvpFlag::kMandatory;
    origin_host_avp.data = encode_octet_string(origin_host);
    encode_avp(avps_bytes, origin_host_avp);

    Avp origin_realm_avp;
    origin_realm_avp.code = dictionary::Avp::kOriginRealm;
    origin_realm_avp.flags = AvpFlag::kMandatory;
    origin_realm_avp.data = encode_octet_string(origin_realm);
    encode_avp(avps_bytes, origin_realm_avp);

    Header answer_header;
    answer_header.flags = 0; // R bit cleared: this is an Answer, not a Request
    answer_header.command_code = dictionary::Command::kSessionTermination;
    answer_header.application_id = request_header.application_id;
    answer_header.hop_by_hop_id = request_header.hop_by_hop_id; // real spec: echoed from request
    answer_header.end_to_end_id = request_header.end_to_end_id; // real spec: echoed from request

    auto message = encode_header(answer_header, static_cast<std::uint32_t>(avps_bytes.size()));
    message.insert(message.end(), avps_bytes.begin(), avps_bytes.end());
    return message;
}

} // namespace

DiameterServer::DiameterServer(
    std::uint16_t port,
    std::string origin_host,
    std::string origin_realm,
    sbi_core::http2::TlsConfig client_tls,
    ChargingDataStore& charging_data_store,
    CdrWriter& cdr_writer,
    RatingDecisionStore& rating_decision_store,
    OfflineChargingDataStore& offline_charging_data_store,
    SpendingLimitSubscriptionStore& spending_limit_store,
    PolicyCounterConfigStore& policy_counter_config_store,
    opentelemetry::metrics::Counter<std::uint64_t>* grant_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* ccr_initial_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* ccr_update_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* ccr_termination_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* acr_event_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* acr_start_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* acr_interim_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* acr_stop_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* slr_initial_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* slr_intermediate_counter,
    opentelemetry::metrics::Counter<std::uint64_t>* str_counter)
    : ioc_(), acceptor_(ioc_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
      origin_host_(std::move(origin_host)), origin_realm_(std::move(origin_realm)),
      client_tls_(std::move(client_tls)), charging_data_store_(charging_data_store),
      cdr_writer_(cdr_writer), rating_decision_store_(rating_decision_store),
      offline_charging_data_store_(offline_charging_data_store),
      spending_limit_store_(spending_limit_store),
      policy_counter_config_store_(policy_counter_config_store), grant_counter_(grant_counter),
      reserve_rejected_counter_(reserve_rejected_counter),
      ccr_initial_counter_(ccr_initial_counter), ccr_update_counter_(ccr_update_counter),
      ccr_termination_counter_(ccr_termination_counter), acr_event_counter_(acr_event_counter),
      acr_start_counter_(acr_start_counter), acr_interim_counter_(acr_interim_counter),
      acr_stop_counter_(acr_stop_counter), slr_initial_counter_(slr_initial_counter),
      slr_intermediate_counter_(slr_intermediate_counter), str_counter_(str_counter) {
    accept_thread_ = std::thread(&DiameterServer::accept_loop, this);
}

DiameterServer::~DiameterServer() {
    stop_ = true;
    boost::system::error_code ec;
    acceptor_.close(ec);
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void DiameterServer::set_tps_limit(double sustained_tps, double burst_capacity) {
    if (sustained_tps <= 0.0) {
        rate_limit_.store(nullptr, std::memory_order_release);
        rate_limit_owner_.reset();
        return;
    }
    rate_limit_owner_ = std::make_unique<sbi_core::TokenBucket>(sustained_tps, burst_capacity);
    // Release/acquire, not relaxed: a connection thread that sees this pointer must also see the
    // fully-constructed bucket behind it.
    rate_limit_.store(rate_limit_owner_.get(), std::memory_order_release);
}

void DiameterServer::accept_loop() {
    while (!stop_) {
        boost::asio::ip::tcp::socket socket(ioc_);
        boost::system::error_code ec;
        acceptor_.accept(socket, ec);
        if (ec) {
            if (!stop_) {
                spdlog::warn("chf: Diameter accept() failed: {}", ec.message());
            }
            continue;
        }
        std::thread(&DiameterServer::handle_connection, this, std::move(socket)).detach();
    }
}

void DiameterServer::handle_connection(boost::asio::ip::tcp::socket socket) {
    boost::system::error_code ec;
    const auto peer = socket.remote_endpoint(ec);
    spdlog::info("chf: Diameter peer connected: {}",
                 ec ? std::string("unknown") : peer.address().to_string());

    std::vector<std::uint8_t> header_bytes(20);
    boost::asio::read(socket, boost::asio::buffer(header_bytes), ec);
    if (ec) {
        spdlog::warn("chf: Diameter connection closed before a full header arrived: {}",
                     ec.message());
        return;
    }

    std::size_t offset = 0;
    std::uint32_t avps_length = 0;
    const auto header = decode_header(header_bytes, offset, avps_length);
    if (!header.has_value()) {
        spdlog::warn("chf: Diameter peer sent a malformed message header, closing connection");
        return;
    }

    std::vector<std::uint8_t> avps_bytes(avps_length);
    if (avps_length > 0) {
        boost::asio::read(socket, boost::asio::buffer(avps_bytes), ec);
        if (ec) {
            spdlog::warn("chf: Diameter connection closed before AVPs arrived: {}", ec.message());
            return;
        }
    }

    if (header->command_code != dictionary::Command::kCapabilitiesExchange ||
        (header->flags & CommandFlag::kRequest) == 0) {
        spdlog::warn(
            "chf: Diameter peer's first message was not a CER (command_code={}, flags={:#x}) -- "
            "closing connection",
            header->command_code,
            header->flags);
        return;
    }

    const auto avps = decode_avps(avps_bytes);
    if (!avps.has_value()) {
        spdlog::warn("chf: Diameter CER had malformed AVPs, closing connection");
        return;
    }

    const auto* peer_origin_host = find_avp(*avps, dictionary::Avp::kOriginHost);
    const auto* peer_origin_realm = find_avp(*avps, dictionary::Avp::kOriginRealm);
    if (peer_origin_host == nullptr || peer_origin_realm == nullptr) {
        spdlog::warn("chf: Diameter CER missing mandatory Origin-Host/Origin-Realm");
        const auto cea = build_cea(
            *header, origin_host_, origin_realm_, dictionary::ResultCode::kDiameterMissingAvp);
        boost::asio::write(socket, boost::asio::buffer(cea), ec);
        return;
    }

    spdlog::info("chf: real CER received from Origin-Host={}",
                 decode_octet_string(peer_origin_host->data).value_or("?"));

    const auto cea =
        build_cea(*header, origin_host_, origin_realm_, dictionary::ResultCode::kDiameterSuccess);
    boost::asio::write(socket, boost::asio::buffer(cea), ec);
    if (ec) {
        spdlog::warn("chf: failed to send CEA: {}", ec.message());
        return;
    }
    spdlog::info("chf: real CEA sent (DIAMETER_SUCCESS) -- connection stays open for real CCR/CCA "
                 "(P4.5/ADR-0060 Stage 3)");

    // P4.5/ADR-0060 Stage 3: CHF as a real HTTP client of bss/product-catalog and
    // bss/balance-management, dedicated to THIS connection's own thread -- see
    // diameter_server.hpp's own header for why this can't reuse main()'s HTTP-route-handler
    // catalog_client/balance_client (sbi_core::http2::Client's real "one instance per thread"
    // contract).
    sbi_core::http2::Client catalog_client(client_tls_);
    sbi_core::http2::Client balance_client(client_tls_);

    // Real Diameter Session-Id -> this project's own ChargingDataRef, scoped to this connection
    // (a real Diameter peer may multiplex many concurrent Gy sessions over one long-lived
    // transport connection, RFC 6733's own expected deployment shape) -- populated on CCR-Initial,
    // consulted on CCR-Update/CCR-Termination, erased on CCR-Termination. No cross-connection
    // sharing: a real peer reconnect starts fresh sessions, same as a real OCS would see.
    std::unordered_map<std::string, std::string> session_to_ref;

    // Same real per-connection Session-Id -> ref scoping as session_to_ref above, for the real Rf
    // ACR/ACA path (ADR-0059 Stage 4) onto Nchf_OfflineOnlyCharging's own OfflineChargingDataStore
    // refs -- a separate map since Gy and Rf are real, independent Diameter sessions/applications,
    // never sharing a ref even if a peer happened to reuse the same Session-Id string.
    std::unordered_map<std::string, std::string> offline_session_to_ref;

    // Same real per-connection Session-Id -> id scoping as the two maps above, for the real Sy
    // SLR/STR path (ADR-0059 Stage 4) onto Nchf_SpendingLimitControl's own subscriptionId.
    std::unordered_map<std::string, std::string> sl_session_to_id;

    while (!stop_) {
        std::vector<std::uint8_t> next_header_bytes(20);
        boost::asio::read(socket, boost::asio::buffer(next_header_bytes), ec);
        if (ec) {
            spdlog::info("chf: Diameter peer disconnected: {}", ec.message());
            return;
        }

        std::size_t next_offset = 0;
        std::uint32_t next_avps_length = 0;
        const auto next_header = decode_header(next_header_bytes, next_offset, next_avps_length);
        if (!next_header.has_value()) {
            spdlog::warn("chf: Diameter peer sent a malformed message header, closing connection");
            return;
        }

        std::vector<std::uint8_t> next_avps_bytes(next_avps_length);
        if (next_avps_length > 0) {
            boost::asio::read(socket, boost::asio::buffer(next_avps_bytes), ec);
            if (ec) {
                spdlog::warn("chf: Diameter connection closed before AVPs arrived: {}",
                             ec.message());
                return;
            }
        }

        const bool is_known_command =
            next_header->command_code == dictionary::Command::kCreditControl ||
            next_header->command_code == dictionary::Command::kAccounting ||
            next_header->command_code == dictionary::Command::kSpendingLimit ||
            next_header->command_code == dictionary::Command::kSessionTermination;
        if (!is_known_command || (next_header->flags & CommandFlag::kRequest) == 0) {
            // Stage 3/4 scope (ADR-0059/ADR-0060): only real CCR-I/U/T (Gy), ACR (Rf, Event/
            // Start/Interim/Stop), and SLR/STR (Sy) are handled -- DWR/DPR and any other real
            // Diameter command close the connection with a warning, same disclosed-gap pattern
            // Stage 2 already used for "first message not CER".
            spdlog::warn("chf: Diameter peer sent an unsupported message (command_code={}, "
                         "flags={:#x}) -- closing connection (only real CCR/ACR/SLR/STR are "
                         "handled)",
                         next_header->command_code,
                         next_header->flags);
            return;
        }

        // P15 (ADR-0285): the Diameter front door's TPS ceiling. Checked once the message is known
        // to be a supported request -- an unsupported command is a peer error, not load -- and
        // before the AVPs are decoded and the charging engine is entered, because shedding is only
        // protective if it is cheaper than serving.
        if (auto* limiter = rate_limit_.load(std::memory_order_acquire);
            limiter != nullptr && !limiter->try_acquire()) {
            // ADR-0291: DIAMETER_TOO_BUSY (3004), the real transient-failure code for overload --
            // a peer backs off and may fail over, rather than treating the request as permanently
            // rejected. ADR-0285 shipped kDiameterUnableToComply (5012) instead because the value
            // could not be verified from material believed to be in hand; it is in fact at
            // simulators/reference/freeDiameter/include/libfdproto.h:1860, the same vendored
            // reference every other Result-Code in this project's dictionary already cites.
            spdlog::warn("chf: Diameter request shed at the configured TPS ceiling "
                         "(command_code={})",
                         next_header->command_code);
            std::vector<std::uint8_t> busy_avps_bytes;
            Avp busy_result;
            busy_result.code = dictionary::Avp::kResultCode;
            busy_result.flags = AvpFlag::kMandatory;
            busy_result.data = encode_integer32(dictionary::ResultCode::kDiameterTooBusy);
            encode_avp(busy_avps_bytes, busy_result);

            Header busy_header{};
            busy_header.command_code = next_header->command_code;
            busy_header.application_id = next_header->application_id;
            busy_header.hop_by_hop_id = next_header->hop_by_hop_id; // echoed, as every answer does
            busy_header.end_to_end_id = next_header->end_to_end_id;
            auto busy =
                encode_header(busy_header, static_cast<std::uint32_t>(busy_avps_bytes.size()));
            busy.insert(busy.end(), busy_avps_bytes.begin(), busy_avps_bytes.end());
            boost::asio::write(socket, boost::asio::buffer(busy), ec);
            if (ec) {
                return;
            }
            continue;
        }

        const auto next_avps = decode_avps(next_avps_bytes);
        if (!next_avps.has_value()) {
            spdlog::warn("chf: Diameter command_code={} had malformed AVPs, closing connection",
                         next_header->command_code);
            return;
        }

        if (next_header->command_code == dictionary::Command::kAccounting) {
            // P4.5/ADR-0059 Stage 4 (Rf half): real ACR dispatched onto
            // Nchf_OfflineOnlyCharging's own real Create/Update/Release (offline_charging_data_
            // store) -- no rating engine involved (Nchf_OfflineOnlyCharging never had one, see
            // main.cpp's own header), so unlike CCR there is no chf::charge_one_usage call here.
            const auto acr = decode_acr(*next_avps);
            if (!acr.has_value()) {
                spdlog::warn("chf: Diameter ACR missing a mandatory AVP (Session-Id/"
                             "Accounting-Record-Type/Accounting-Record-Number)");
                std::string best_effort_session_id;
                if (const auto* sid = find_avp(*next_avps, dictionary::Avp::kSessionId);
                    sid != nullptr) {
                    best_effort_session_id = decode_octet_string(sid->data).value_or("");
                }
                const auto aca = build_aca(*next_header,
                                           origin_host_,
                                           origin_realm_,
                                           best_effort_session_id,
                                           0,
                                           0,
                                           dictionary::ResultCode::kDiameterMissingAvp);
                boost::asio::write(socket, boost::asio::buffer(aca), ec);
                if (ec) {
                    spdlog::warn("chf: failed to send ACA: {}", ec.message());
                    return;
                }
                continue;
            }

            spdlog::info("chf: real ACR received (Session-Id={}, Accounting-Record-Type={}, "
                         "Accounting-Record-Number={})",
                         acr->session_id,
                         acr->accounting_record_type,
                         acr->accounting_record_number);

            std::int32_t result_code = dictionary::ResultCode::kDiameterSuccess;
            using dictionary::BaseAccounting::AccountingRecordType::kEvent;
            using dictionary::BaseAccounting::AccountingRecordType::kInterim;
            using dictionary::BaseAccounting::AccountingRecordType::kStart;
            using dictionary::BaseAccounting::AccountingRecordType::kStop;

            if (acr->accounting_record_type == kStart) {
                offline_session_to_ref[acr->session_id] = offline_charging_data_store_.create();
                if (acr_start_counter_ != nullptr) {
                    acr_start_counter_->Add(1);
                }
            } else if (acr->accounting_record_type == kEvent) {
                // Real RFC 6733 semantics: EVENT_RECORD is a single, self-contained accounting
                // event, not part of a Start/Interim/Stop session -- create then immediately
                // release, matching Nchf_OfflineOnlyCharging's own Create/Release pair rather
                // than leaving a session open with nothing to ever close it.
                const auto ref = offline_charging_data_store_.create();
                offline_charging_data_store_.release(ref);
                if (acr_event_counter_ != nullptr) {
                    acr_event_counter_->Add(1);
                }
            } else if (acr->accounting_record_type == kInterim) {
                const auto it = offline_session_to_ref.find(acr->session_id);
                if (it == offline_session_to_ref.end() ||
                    !offline_charging_data_store_.is_active(it->second)) {
                    spdlog::warn("chf: Diameter ACR (INTERIM_RECORD) for unknown Session-Id={}",
                                 acr->session_id);
                    result_code = dictionary::ResultCode::kDiameterUnknownSessionId;
                } else if (acr_interim_counter_ != nullptr) {
                    acr_interim_counter_->Add(1);
                }
            } else if (acr->accounting_record_type == kStop) {
                const auto it = offline_session_to_ref.find(acr->session_id);
                if (it == offline_session_to_ref.end() ||
                    !offline_charging_data_store_.release(it->second)) {
                    spdlog::warn("chf: Diameter ACR (STOP_RECORD) for unknown Session-Id={}",
                                 acr->session_id);
                    result_code = dictionary::ResultCode::kDiameterUnknownSessionId;
                } else {
                    offline_session_to_ref.erase(it);
                    if (acr_stop_counter_ != nullptr) {
                        acr_stop_counter_->Add(1);
                    }
                }
            } else {
                spdlog::warn("chf: Diameter ACR with unrecognized Accounting-Record-Type={}",
                             acr->accounting_record_type);
                result_code = dictionary::ResultCode::kDiameterUnableToComply;
            }

            const auto aca = build_aca(*next_header,
                                       origin_host_,
                                       origin_realm_,
                                       acr->session_id,
                                       acr->accounting_record_type,
                                       acr->accounting_record_number,
                                       result_code);
            boost::asio::write(socket, boost::asio::buffer(aca), ec);
            if (ec) {
                spdlog::warn("chf: failed to send ACA: {}", ec.message());
                return;
            }
            continue;
        }

        if (next_header->command_code == dictionary::Command::kSpendingLimit) {
            // P4.5/ADR-0059 Stage 4 (Sy half): real SLR dispatched onto
            // Nchf_SpendingLimitControl's own real Subscribe/Update (spending_limit_store_) and
            // chf::build_spending_limit_status (charging_engine.hpp) -- the same single-code-path
            // property Gy/Rf already established, applied to Sy.
            const auto slr = decode_slr(*next_avps);
            if (!slr.has_value()) {
                spdlog::warn("chf: Diameter SLR missing a mandatory AVP (Session-Id/"
                             "SL-Request-Type)");
                std::string best_effort_session_id;
                if (const auto* sid = find_avp(*next_avps, dictionary::Avp::kSessionId);
                    sid != nullptr) {
                    best_effort_session_id = decode_octet_string(sid->data).value_or("");
                }
                const auto sla = build_sla(*next_header,
                                           origin_host_,
                                           origin_realm_,
                                           best_effort_session_id,
                                           dictionary::ResultCode::kDiameterMissingAvp,
                                           {});
                boost::asio::write(socket, boost::asio::buffer(sla), ec);
                if (ec) {
                    spdlog::warn("chf: failed to send SLA: {}", ec.message());
                    return;
                }
                continue;
            }

            spdlog::info("chf: real SLR received (Session-Id={}, SL-Request-Type={}, SUPI={}, "
                         "{} Policy-Counter-Identifier(s))",
                         slr->session_id,
                         slr->sl_request_type,
                         slr->supi.empty() ? "?" : slr->supi,
                         slr->policy_counter_ids.size());

            sbi_gen::SpendingLimitContext context{};
            if (!slr->supi.empty()) {
                context.supi = slr->supi;
            }
            if (!slr->policy_counter_ids.empty()) {
                context.policyCounterIds = slr->policy_counter_ids;
            }

            std::int32_t result_code = dictionary::ResultCode::kDiameterSuccess;
            std::vector<std::pair<std::string, std::string>> policy_counter_status;
            const auto status =
                chf::build_spending_limit_status(context, policy_counter_config_store_);
            if (status.statusInfos.has_value() && status.statusInfos->is_object()) {
                for (const auto& [counter_id, info] : status.statusInfos->items()) {
                    policy_counter_status.emplace_back(
                        counter_id, info.value("currentStatus", std::string("unknown")));
                }
            }

            if (slr->sl_request_type == dictionary::Sy::SlRequestType::kInitial) {
                sl_session_to_id[slr->session_id] = spending_limit_store_.create(context);
                if (slr_initial_counter_ != nullptr) {
                    slr_initial_counter_->Add(1);
                }
            } else if (slr->sl_request_type == dictionary::Sy::SlRequestType::kIntermediate) {
                const auto it = sl_session_to_id.find(slr->session_id);
                if (it == sl_session_to_id.end() ||
                    !spending_limit_store_.update(it->second, context)) {
                    spdlog::warn("chf: Diameter SLR (INTERMEDIATE_REQUEST) for unknown "
                                 "Session-Id={}",
                                 slr->session_id);
                    result_code = dictionary::ResultCode::kDiameterUnknownSessionId;
                } else if (slr_intermediate_counter_ != nullptr) {
                    slr_intermediate_counter_->Add(1);
                }
            } else {
                spdlog::warn("chf: Diameter SLR with unrecognized SL-Request-Type={}",
                             slr->sl_request_type);
                result_code = dictionary::ResultCode::kDiameterUnableToComply;
            }

            const auto sla = build_sla(*next_header,
                                       origin_host_,
                                       origin_realm_,
                                       slr->session_id,
                                       result_code,
                                       result_code == dictionary::ResultCode::kDiameterSuccess
                                           ? policy_counter_status
                                           : std::vector<std::pair<std::string, std::string>>{});
            boost::asio::write(socket, boost::asio::buffer(sla), ec);
            if (ec) {
                spdlog::warn("chf: failed to send SLA: {}", ec.message());
                return;
            }
            continue;
        }

        if (next_header->command_code == dictionary::Command::kSessionTermination) {
            // P4.5/ADR-0059 Stage 4 (Sy half): real STR (TS 29.219's own real Final Spending
            // Limit Report Request, §4.5.3.1) dispatched onto Nchf_SpendingLimitControl's own
            // real Unsubscribe (spending_limit_store_.remove).
            const auto str = decode_str(*next_avps);
            if (!str.has_value()) {
                spdlog::warn("chf: Diameter STR missing a mandatory AVP (Session-Id/"
                             "Termination-Cause)");
                std::string best_effort_session_id;
                if (const auto* sid = find_avp(*next_avps, dictionary::Avp::kSessionId);
                    sid != nullptr) {
                    best_effort_session_id = decode_octet_string(sid->data).value_or("");
                }
                const auto sta = build_sta(*next_header,
                                           origin_host_,
                                           origin_realm_,
                                           best_effort_session_id,
                                           dictionary::ResultCode::kDiameterMissingAvp);
                boost::asio::write(socket, boost::asio::buffer(sta), ec);
                if (ec) {
                    spdlog::warn("chf: failed to send STA: {}", ec.message());
                    return;
                }
                continue;
            }

            spdlog::info("chf: real STR received (Session-Id={}, Termination-Cause={})",
                         str->session_id,
                         str->termination_cause);

            std::int32_t result_code = dictionary::ResultCode::kDiameterSuccess;
            const auto it = sl_session_to_id.find(str->session_id);
            if (it == sl_session_to_id.end() || !spending_limit_store_.remove(it->second)) {
                spdlog::warn("chf: Diameter STR for unknown Session-Id={}", str->session_id);
                result_code = dictionary::ResultCode::kDiameterUnknownSessionId;
            } else {
                sl_session_to_id.erase(it);
                if (str_counter_ != nullptr) {
                    str_counter_->Add(1);
                }
            }

            const auto sta =
                build_sta(*next_header, origin_host_, origin_realm_, str->session_id, result_code);
            boost::asio::write(socket, boost::asio::buffer(sta), ec);
            if (ec) {
                spdlog::warn("chf: failed to send STA: {}", ec.message());
                return;
            }
            continue;
        }

        const auto ccr = decode_ccr(*next_avps);
        if (!ccr.has_value()) {
            spdlog::warn("chf: Diameter CCR missing a mandatory AVP (Session-Id/CC-Request-Type/"
                         "CC-Request-Number)");
            // Best-effort echo: a real Session-Id may still be present even if CC-Request-Type/
            // Number are not (or vice versa) -- send back whatever can be recovered rather than
            // nothing, same "answer every request" real Diameter peer expectation the loop's other
            // error paths follow.
            std::string best_effort_session_id;
            if (const auto* sid = find_avp(*next_avps, dictionary::Avp::kSessionId);
                sid != nullptr) {
                best_effort_session_id = decode_octet_string(sid->data).value_or("");
            }
            const auto cca = build_cca(*next_header,
                                       origin_host_,
                                       origin_realm_,
                                       best_effort_session_id,
                                       0,
                                       0,
                                       dictionary::ResultCode::kDiameterMissingAvp,
                                       {});
            boost::asio::write(socket, boost::asio::buffer(cca), ec);
            continue;
        }

        spdlog::info("chf: real CCR received (Session-Id={}, CC-Request-Type={}, "
                     "CC-Request-Number={}, SUPI={})",
                     ccr->session_id,
                     ccr->cc_request_type,
                     ccr->cc_request_number,
                     ccr->supi.empty() ? "?" : ccr->supi);

        if (ccr->cc_request_type == dictionary::Dcc::CcRequestType::kInitial) {
            const auto ref = charging_data_store_.create(ccr->supi);
            session_to_ref[ccr->session_id] = ref;
            if (ccr_initial_counter_ != nullptr) {
                ccr_initial_counter_->Add(1);
            }

            std::vector<std::pair<std::uint32_t, std::optional<sbi_gen::GrantedUnit>>> granted;
            for (const auto& mscc : ccr->mscc) {
                sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging usage{};
                usage.ratingGroup = mscc.rating_group;
                // The single, shared code path -- identical to Nchf_ConvergedCharging's own HTTP
                // Create handler (main.cpp), per CHARGING_PROMPT.md's explicit P4.5 requirement.
                const auto charged =
                    chf::charge_one_usage(catalog_client,
                                          balance_client,
                                          cdr_writer_,
                                          rating_decision_store_,
                                          charging_data_store_,
                                          grant_counter_,
                                          reserve_rejected_counter_,
                                          ref,
                                          "Create",
                                          ccr->supi,
                                          "Diameter-Gy",
                                          // Real, disclosed: same reasoning as cap_server.cpp's
                                          // own call site -- "Diameter-Gy" is a protocol-identity
                                          // label, not a real TS 32.298 NetworkFunctionality
                                          // value, so this call path's own asn1_cdr blob is
                                          // already empty regardless of this field's value.
                                          "",
                                          static_cast<std::int64_t>(ccr->cc_request_number),
                                          usage);
                granted.emplace_back(mscc.rating_group,
                                     charged.reserved ? charged.rating.grant : std::nullopt);
            }
            const auto cca = build_cca(*next_header,
                                       origin_host_,
                                       origin_realm_,
                                       ccr->session_id,
                                       ccr->cc_request_type,
                                       ccr->cc_request_number,
                                       dictionary::ResultCode::kDiameterSuccess,
                                       granted);
            boost::asio::write(socket, boost::asio::buffer(cca), ec);
        } else if (ccr->cc_request_type == dictionary::Dcc::CcRequestType::kUpdate) {
            const auto it = session_to_ref.find(ccr->session_id);
            if (it == session_to_ref.end()) {
                spdlog::warn("chf: Diameter CCR-Update for unknown Session-Id={}", ccr->session_id);
                const auto cca = build_cca(*next_header,
                                           origin_host_,
                                           origin_realm_,
                                           ccr->session_id,
                                           ccr->cc_request_type,
                                           ccr->cc_request_number,
                                           dictionary::ResultCode::kDiameterUnknownSessionId,
                                           {});
                boost::asio::write(socket, boost::asio::buffer(cca), ec);
                continue;
            }
            const auto& ref = it->second;
            const auto supi = charging_data_store_.get_supi(ref).value_or(ccr->supi);
            if (ccr_update_counter_ != nullptr) {
                ccr_update_counter_->Add(1);
            }

            std::vector<std::pair<std::uint32_t, std::optional<sbi_gen::GrantedUnit>>> granted;
            for (const auto& mscc : ccr->mscc) {
                sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging usage{};
                usage.ratingGroup = mscc.rating_group;
                if (mscc.used_total_octets.has_value()) {
                    sbi_gen::UsedUnitContainer_Nchf_ConvergedCharging used{};
                    used.totalVolume = static_cast<std::int64_t>(*mscc.used_total_octets);
                    // Real, disclosed mapping: RFC 4006 has no per-container sequence-number AVP
                    // equivalent to TS 32.291's localSequenceNumber -- CC-Request-Number (this
                    // session's own real Diameter monotonic counter) is the closest real
                    // available value, not fabricated.
                    used.localSequenceNumber = static_cast<std::int64_t>(ccr->cc_request_number);
                    usage.usedUnitContainer =
                        std::vector<sbi_gen::UsedUnitContainer_Nchf_ConvergedCharging>{used};
                }
                const auto charged =
                    chf::charge_one_usage(catalog_client,
                                          balance_client,
                                          cdr_writer_,
                                          rating_decision_store_,
                                          charging_data_store_,
                                          grant_counter_,
                                          reserve_rejected_counter_,
                                          ref,
                                          "Update",
                                          supi,
                                          "Diameter-Gy",
                                          "", // real, disclosed -- see the Create call site's own
                                              // comment above.
                                          static_cast<std::int64_t>(ccr->cc_request_number),
                                          usage);
                granted.emplace_back(mscc.rating_group,
                                     charged.reserved ? charged.rating.grant : std::nullopt);
            }
            const auto cca = build_cca(*next_header,
                                       origin_host_,
                                       origin_realm_,
                                       ccr->session_id,
                                       ccr->cc_request_type,
                                       ccr->cc_request_number,
                                       dictionary::ResultCode::kDiameterSuccess,
                                       granted);
            boost::asio::write(socket, boost::asio::buffer(cca), ec);
        } else if (ccr->cc_request_type == dictionary::Dcc::CcRequestType::kTermination) {
            const auto it = session_to_ref.find(ccr->session_id);
            if (it == session_to_ref.end()) {
                spdlog::warn("chf: Diameter CCR-Termination for unknown Session-Id={}",
                             ccr->session_id);
                const auto cca = build_cca(*next_header,
                                           origin_host_,
                                           origin_realm_,
                                           ccr->session_id,
                                           ccr->cc_request_type,
                                           ccr->cc_request_number,
                                           dictionary::ResultCode::kDiameterUnknownSessionId,
                                           {});
                boost::asio::write(socket, boost::asio::buffer(cca), ec);
                continue;
            }
            const auto ref = it->second;

            // Mirrors Nchf_ConvergedCharging_Release's own real logic (main.cpp) exactly: finalize
            // (real permanent debit + unreserve) the session's full reserved total, release the
            // ref, then a single final CDR row -- not a per-rating-group charge_one_usage call,
            // same real reasoning as the HTTP Release handler (RFC 4006's own CCR-T reports final
            // usage but does not request new units, so there is no new grant to rate).
            const auto supi = charging_data_store_.get_supi(ref);
            const auto reserved_total = charging_data_store_.get_reserved_total(ref);
            const auto granted_volume = charging_data_store_.get_granted_volume(ref);
            const auto granted_service_units = charging_data_store_.get_granted_service_units(ref);
            charging_data_store_.release(ref);
            // ADR-0297: RFC 4006's CCR-Termination reports final usage in Used-Service-Unit's
            // CC-Total-Octets, which this handler has always decoded (DecodedMscc) and, until now,
            // used only for the CDR. It is the Gy equivalent of TS 32.291's usedUnitContainer
            // totalVolume, so it proportions the debit the same way.
            double used_volume = 0.0;
            for (const auto& mscc : ccr->mscc) {
                used_volume += static_cast<double>(mscc.used_total_octets.value_or(0));
            }
            // Gy reports octets only -- no service-unit counterpart is decoded on this path, so
            // that dimension is passed as zero-used rather than invented.
            const auto amount_to_debit = chf::proportional_debit(
                reserved_total, granted_volume, used_volume, granted_service_units, 0.0);
            if (supi.has_value() && !supi->empty()) {
                spdlog::info("chf: Gy CCR-T finalize for {} -- reserved {}, debiting {} (used {} "
                             "of {} octets)",
                             ref,
                             reserved_total,
                             amount_to_debit,
                             used_volume,
                             granted_volume);
                chf::finalize_subscriber_balance(balance_client,
                                                 *supi,
                                                 reserved_total,
                                                 amount_to_debit,
                                                 "Diameter-Gy CCR-Termination " + ref);
            }
            // ADR-0192: CdrWriter::write() catches every real Doris error surface internally and
            // logs a warning -- it never throws, so no try/catch is needed here (unlike the
            // pre-migration version; see ADR-0192).
            chf::CdrRecord cdr{};
            cdr.charging_data_ref = ref;
            cdr.invocation_sequence_number = static_cast<std::int64_t>(ccr->cc_request_number);
            cdr.service_type = "ConvergedCharging";
            cdr.operation = "Release";
            cdr.subscriber_identifier = supi.value_or("");
            cdr.nf_consumer_node_functionality = "Diameter-Gy";
            if (reserved_total > 0.0) {
                cdr.reserved_cost = reserved_total;
            }
            // Real, disclosed fallback (not the HTTP path's behavior): RFC 4006 carries an
            // Event-Timestamp AVP, but this build's CCR decoding does not extract one, so there
            // is no consumer-supplied event time to record here and write time is used instead.
            // The SBI path records the real TS 32.291 invocationTimeStamp -- see
            // write_converged_charging_cdr's own header comment.
            cdr.invocation_time_stamp =
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            cdr_writer_.write(cdr);
            session_to_ref.erase(it);
            if (ccr_termination_counter_ != nullptr) {
                ccr_termination_counter_->Add(1);
            }

            const auto cca = build_cca(*next_header,
                                       origin_host_,
                                       origin_realm_,
                                       ccr->session_id,
                                       ccr->cc_request_type,
                                       ccr->cc_request_number,
                                       dictionary::ResultCode::kDiameterSuccess,
                                       {});
            boost::asio::write(socket, boost::asio::buffer(cca), ec);
        } else {
            // CC-Request-Type EVENT_REQUEST (4) or any other value: real, disclosed gap -- this
            // Stage's scope (task-tracked, ADR-0060) is CCR-I/U/T only, mirroring
            // Nchf_ConvergedCharging's own Create/Update/Release shape. Event-based (one-shot,
            // sessionless) credit-control has no HTTP-path analogue in this codebase to share a
            // code path with, so it is not implemented here rather than given a fabricated one.
            spdlog::warn("chf: Diameter CCR with unsupported CC-Request-Type={} (only "
                         "INITIAL/UPDATE/TERMINATION are handled, ADR-0060 Stage 3)",
                         ccr->cc_request_type);
            const auto cca = build_cca(*next_header,
                                       origin_host_,
                                       origin_realm_,
                                       ccr->session_id,
                                       ccr->cc_request_type,
                                       ccr->cc_request_number,
                                       dictionary::ResultCode::kDiameterUnableToComply,
                                       {});
            boost::asio::write(socket, boost::asio::buffer(cca), ec);
        }

        if (ec) {
            spdlog::warn("chf: failed to send CCA: {}", ec.message());
            return;
        }
    }
}

} // namespace chf
