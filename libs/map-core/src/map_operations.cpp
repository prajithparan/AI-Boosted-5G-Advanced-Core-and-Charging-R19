#include "map_core/map_operations.hpp"

namespace map_core {

using tcap_core::decode_integer;
using tcap_core::decode_tlv;
using tcap_core::decode_tlvs;
using tcap_core::encode_integer;
using tcap_core::encode_tlv;
using tcap_core::TagClass;
using tcap_core::Tlv;
namespace UniversalTag = tcap_core::UniversalTag;

namespace {

Tlv make_primitive(TagClass cls, std::uint32_t tag, std::vector<std::uint8_t> value) {
    Tlv t;
    t.tag_class = cls;
    t.constructed = false;
    t.tag_number = tag;
    t.value = std::move(value);
    return t;
}

Tlv make_int_tlv(TagClass cls, std::uint32_t tag, std::int32_t v) {
    return make_primitive(cls, tag, encode_integer(v));
}

Tlv make_context_constructed(std::uint32_t tag, std::vector<std::uint8_t> value) {
    Tlv t;
    t.tag_class = TagClass::kContext;
    t.constructed = true;
    t.tag_number = tag;
    t.value = std::move(value);
    return t;
}

Tlv make_sequence(std::vector<std::uint8_t> body) {
    Tlv t;
    t.tag_class = TagClass::kUniversal;
    t.constructed = true;
    t.tag_number = UniversalTag::kSequence;
    t.value = std::move(body);
    return t;
}

std::vector<std::uint8_t> wrap_top_level(const std::vector<Tlv>& fields) {
    std::vector<std::uint8_t> body;
    for (const auto& f : fields) {
        encode_tlv(body, f);
    }
    std::vector<std::uint8_t> out;
    encode_tlv(out, make_sequence(body));
    return out;
}

std::optional<std::vector<Tlv>> unwrap_top_level(const std::vector<std::uint8_t>& parameter) {
    std::size_t offset = 0;
    const auto tlv = decode_tlv(parameter, offset);
    if (!tlv.has_value() || tlv->tag_class != TagClass::kUniversal ||
        tlv->tag_number != UniversalTag::kSequence) {
        return std::nullopt;
    }
    return decode_tlvs(tlv->value);
}

const Tlv* find_tag(const std::vector<Tlv>& parts, TagClass cls, std::uint32_t tag) {
    for (const auto& p : parts) {
        if (p.tag_class == cls && p.tag_number == tag) {
            return &p;
        }
    }
    return nullptr;
}

// --- O-BcsmCamelTDPData: real field order is fixed and positional (see this header's own note:
// the first two fields are untagged, retaining their type's own universal tag). ---

Tlv encode_o_bcsm_camel_tdp_data(const OBcsmCamelTdpData& d) {
    std::vector<std::uint8_t> body;
    encode_tlv(
        body,
        make_int_tlv(TagClass::kUniversal, UniversalTag::kEnumerated, d.trigger_detection_point));
    encode_tlv(body, make_int_tlv(TagClass::kUniversal, UniversalTag::kInteger, d.service_key));
    encode_tlv(body, make_primitive(TagClass::kContext, 0, d.gsm_scf_address));
    encode_tlv(body, make_int_tlv(TagClass::kContext, 1, d.default_call_handling));
    return make_sequence(body);
}

std::optional<OBcsmCamelTdpData> decode_o_bcsm_camel_tdp_data(const Tlv& tlv) {
    if (tlv.tag_class != TagClass::kUniversal || tlv.tag_number != UniversalTag::kSequence) {
        return std::nullopt;
    }
    const auto parts = decode_tlvs(tlv.value);
    if (!parts.has_value() || parts->size() < 4) {
        return std::nullopt;
    }
    const auto& p = *parts;
    if (p[0].tag_class != TagClass::kUniversal || p[0].tag_number != UniversalTag::kEnumerated ||
        p[1].tag_class != TagClass::kUniversal || p[1].tag_number != UniversalTag::kInteger ||
        p[2].tag_class != TagClass::kContext || p[2].tag_number != 0 ||
        p[3].tag_class != TagClass::kContext || p[3].tag_number != 1) {
        return std::nullopt;
    }
    const auto tdp = decode_integer(p[0].value);
    const auto sk = decode_integer(p[1].value);
    const auto dch = decode_integer(p[3].value);
    if (!tdp.has_value() || !sk.has_value() || !dch.has_value()) {
        return std::nullopt;
    }
    OBcsmCamelTdpData d;
    d.trigger_detection_point = *tdp;
    d.service_key = *sk;
    d.gsm_scf_address = p[2].value;
    d.default_call_handling = *dch;
    return d;
}

// --- O-CSI ---

Tlv encode_o_csi(const OCsi& c) {
    std::vector<std::uint8_t> list_body;
    for (const auto& d : c.tdp_data_list) {
        encode_tlv(list_body, encode_o_bcsm_camel_tdp_data(d));
    }

    std::vector<std::uint8_t> body;
    encode_tlv(body, make_sequence(list_body)); // untagged -> retains universal SEQUENCE tag
    if (c.camel_capability_handling.has_value()) {
        encode_tlv(body, make_int_tlv(TagClass::kContext, 0, *c.camel_capability_handling));
    }
    return make_sequence(body);
}

std::optional<OCsi> decode_o_csi(const Tlv& tlv) {
    const auto parts = decode_tlvs(tlv.value);
    if (!parts.has_value() || parts->empty()) {
        return std::nullopt;
    }
    const auto& p = *parts;
    if (p[0].tag_class != TagClass::kUniversal || p[0].tag_number != UniversalTag::kSequence) {
        return std::nullopt;
    }
    const auto items = decode_tlvs(p[0].value);
    if (!items.has_value()) {
        return std::nullopt;
    }
    OCsi c;
    for (const auto& item : *items) {
        const auto d = decode_o_bcsm_camel_tdp_data(item);
        if (!d.has_value()) {
            return std::nullopt;
        }
        c.tdp_data_list.push_back(*d);
    }
    if (const auto* cch = find_tag(p, TagClass::kContext, 0); cch != nullptr) {
        const auto v = decode_integer(cch->value);
        if (!v.has_value()) {
            return std::nullopt;
        }
        c.camel_capability_handling = *v;
    }
    return c;
}

// --- DP-AnalysedInfoCriterion: positional (two untagged sibling fields, dialledNumber and
// gsmSCF-Address, share the identical universal OCTET STRING tag -- see this header's own note).

Tlv encode_dp_analysed_info_criterion(const DpAnalysedInfoCriterion& c) {
    std::vector<std::uint8_t> body;
    encode_tlv(body,
               make_primitive(TagClass::kUniversal, UniversalTag::kOctetString, c.dialled_number));
    encode_tlv(body, make_int_tlv(TagClass::kUniversal, UniversalTag::kInteger, c.service_key));
    encode_tlv(body,
               make_primitive(TagClass::kUniversal, UniversalTag::kOctetString, c.gsm_scf_address));
    encode_tlv(
        body,
        make_int_tlv(TagClass::kUniversal, UniversalTag::kEnumerated, c.default_call_handling));
    return make_sequence(body);
}

std::optional<DpAnalysedInfoCriterion> decode_dp_analysed_info_criterion(const Tlv& tlv) {
    if (tlv.tag_class != TagClass::kUniversal || tlv.tag_number != UniversalTag::kSequence) {
        return std::nullopt;
    }
    const auto parts = decode_tlvs(tlv.value);
    if (!parts.has_value() || parts->size() < 4) {
        return std::nullopt;
    }
    const auto& p = *parts;
    if (p[0].tag_class != TagClass::kUniversal || p[0].tag_number != UniversalTag::kOctetString ||
        p[1].tag_class != TagClass::kUniversal || p[1].tag_number != UniversalTag::kInteger ||
        p[2].tag_class != TagClass::kUniversal || p[2].tag_number != UniversalTag::kOctetString ||
        p[3].tag_class != TagClass::kUniversal || p[3].tag_number != UniversalTag::kEnumerated) {
        return std::nullopt;
    }
    const auto sk = decode_integer(p[1].value);
    const auto dch = decode_integer(p[3].value);
    if (!sk.has_value() || !dch.has_value()) {
        return std::nullopt;
    }
    DpAnalysedInfoCriterion c;
    c.dialled_number = p[0].value;
    c.service_key = *sk;
    c.gsm_scf_address = p[2].value;
    c.default_call_handling = *dch;
    return c;
}

// --- D-CSI ---

Tlv encode_d_csi(const DCsi& d) {
    std::vector<std::uint8_t> body;
    if (!d.dp_analysed_info_criteria_list.empty()) {
        std::vector<std::uint8_t> list_body;
        for (const auto& c : d.dp_analysed_info_criteria_list) {
            encode_tlv(list_body, encode_dp_analysed_info_criterion(c));
        }
        encode_tlv(body, make_context_constructed(0, list_body));
    }
    if (d.camel_capability_handling.has_value()) {
        encode_tlv(body, make_int_tlv(TagClass::kContext, 1, *d.camel_capability_handling));
    }
    return make_sequence(body);
}

std::optional<DCsi> decode_d_csi(const Tlv& tlv) {
    const auto parts = decode_tlvs(tlv.value);
    if (!parts.has_value()) {
        return std::nullopt;
    }
    DCsi d;
    if (const auto* list = find_tag(*parts, TagClass::kContext, 0); list != nullptr) {
        const auto items = decode_tlvs(list->value);
        if (!items.has_value()) {
            return std::nullopt;
        }
        for (const auto& item : *items) {
            const auto c = decode_dp_analysed_info_criterion(item);
            if (!c.has_value()) {
                return std::nullopt;
            }
            d.dp_analysed_info_criteria_list.push_back(*c);
        }
    }
    if (const auto* cch = find_tag(*parts, TagClass::kContext, 1); cch != nullptr) {
        const auto v = decode_integer(cch->value);
        if (!v.has_value()) {
            return std::nullopt;
        }
        d.camel_capability_handling = *v;
    }
    return d;
}

// --- VlrCamelSubscriptionInfo ---

Tlv encode_vlr_camel_subscription_info(const VlrCamelSubscriptionInfo& v) {
    std::vector<std::uint8_t> body;
    if (v.o_csi.has_value()) {
        const auto inner = encode_o_csi(*v.o_csi);
        encode_tlv(body, make_context_constructed(0, inner.value));
    }
    if (v.d_csi.has_value()) {
        const auto inner = encode_d_csi(*v.d_csi);
        encode_tlv(body, make_context_constructed(9, inner.value));
    }
    return make_sequence(body);
}

std::optional<VlrCamelSubscriptionInfo> decode_vlr_camel_subscription_info(const Tlv& tlv) {
    const auto parts = decode_tlvs(tlv.value);
    if (!parts.has_value()) {
        return std::nullopt;
    }
    VlrCamelSubscriptionInfo v;
    if (const auto* o = find_tag(*parts, TagClass::kContext, 0); o != nullptr) {
        const auto oc = decode_o_csi(*o);
        if (!oc.has_value()) {
            return std::nullopt;
        }
        v.o_csi = oc;
    }
    if (const auto* d = find_tag(*parts, TagClass::kContext, 9); d != nullptr) {
        const auto dc = decode_d_csi(*d);
        if (!dc.has_value()) {
            return std::nullopt;
        }
        v.d_csi = dc;
    }
    return v;
}

} // namespace

std::vector<std::uint8_t> encode_insert_subscriber_data_arg(const InsertSubscriberDataArg& arg) {
    std::vector<Tlv> fields;
    if (arg.imsi.has_value()) {
        fields.push_back(make_primitive(TagClass::kContext, 0, *arg.imsi));
    }
    if (arg.msisdn.has_value()) {
        fields.push_back(make_primitive(TagClass::kContext, 1, *arg.msisdn));
    }
    if (arg.vlr_camel_subscription_info.has_value()) {
        const auto inner = encode_vlr_camel_subscription_info(*arg.vlr_camel_subscription_info);
        fields.push_back(make_context_constructed(13, inner.value));
    }
    return wrap_top_level(fields);
}

std::optional<InsertSubscriberDataArg>
decode_insert_subscriber_data_arg(const std::vector<std::uint8_t>& parameter) {
    const auto parts = unwrap_top_level(parameter);
    if (!parts.has_value()) {
        return std::nullopt;
    }
    InsertSubscriberDataArg arg;
    if (const auto* imsi = find_tag(*parts, TagClass::kContext, 0); imsi != nullptr) {
        arg.imsi = imsi->value;
    }
    if (const auto* msisdn = find_tag(*parts, TagClass::kContext, 1); msisdn != nullptr) {
        arg.msisdn = msisdn->value;
    }
    if (const auto* vlr = find_tag(*parts, TagClass::kContext, 13); vlr != nullptr) {
        const auto v = decode_vlr_camel_subscription_info(*vlr);
        if (!v.has_value()) {
            return std::nullopt;
        }
        arg.vlr_camel_subscription_info = v;
    }
    return arg;
}

std::vector<std::uint8_t> encode_insert_subscriber_data_res() {
    return wrap_top_level({});
}

bool decode_insert_subscriber_data_res(const std::vector<std::uint8_t>& parameter) {
    return unwrap_top_level(parameter).has_value();
}

// --- cancelLocation (ADR-0296) -----------------------------------------------------------------
// See map_operations.hpp for the real ASN.1 and the two ways this differs from
// insertSubscriberData: a CONTEXT [3] CONSTRUCTED wrapper rather than a UNIVERSAL SEQUENCE, and an
// untagged CHOICE decoded positionally.

std::vector<std::uint8_t> encode_cancel_location_arg(const CancelLocationArg& arg) {
    std::vector<std::uint8_t> body;
    // identity CHOICE, imsi arm: an untagged IMSI keeps its own UNIVERSAL OCTET STRING tag, and
    // must come first -- that position is what identifies it.
    encode_tlv(body, make_primitive(TagClass::kUniversal, UniversalTag::kOctetString, arg.imsi));
    if (arg.cancellation_type.has_value()) {
        encode_tlv(body,
                   make_primitive(TagClass::kUniversal,
                                  UniversalTag::kEnumerated,
                                  encode_integer(*arg.cancellation_type)));
    }
    std::vector<std::uint8_t> out;
    encode_tlv(out, make_context_constructed(3, std::move(body)));
    return out;
}

std::optional<CancelLocationArg>
decode_cancel_location_arg(const std::vector<std::uint8_t>& parameter) {
    std::size_t offset = 0;
    const auto outer = decode_tlv(parameter, offset);
    if (!outer.has_value() || outer->tag_class != TagClass::kContext || !outer->constructed ||
        outer->tag_number != 3) {
        return std::nullopt;
    }
    const auto parts = decode_tlvs(outer->value);
    if (!parts.has_value() || parts->empty()) {
        return std::nullopt;
    }

    // identity, positionally first. Only the imsi arm is modeled; an imsi-WithLMSI (a constructed
    // UNIVERSAL SEQUENCE in the same slot) is a real shape this codec does not handle, and is
    // reported as a decode failure rather than silently mistaken for an IMSI.
    const auto& identity = parts->front();
    if (identity.tag_class != TagClass::kUniversal ||
        identity.tag_number != UniversalTag::kOctetString || identity.constructed) {
        return std::nullopt;
    }
    CancelLocationArg arg;
    arg.imsi = identity.value;

    // cancellationType is OPTIONAL and, being untagged, carries its own UNIVERSAL ENUMERATED tag.
    // Every other field is a real part of the structure this codec does not model; they are
    // skipped rather than treated as an error, because a real peer may legitimately send them.
    for (std::size_t i = 1; i < parts->size(); ++i) {
        const auto& part = (*parts)[i];
        if (part.tag_class == TagClass::kUniversal &&
            part.tag_number == UniversalTag::kEnumerated) {
            const auto value = tcap_core::decode_integer(part.value);
            if (!value.has_value()) {
                return std::nullopt;
            }
            arg.cancellation_type = *value;
        }
    }
    return arg;
}

std::vector<std::uint8_t> encode_cancel_location_res() {
    return wrap_top_level({});
}

bool decode_cancel_location_res(const std::vector<std::uint8_t>& parameter) {
    return unwrap_top_level(parameter).has_value();
}

} // namespace map_core
