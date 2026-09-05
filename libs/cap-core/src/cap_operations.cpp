#include "cap_core/cap_operations.hpp"

#include "tcap_core/ber.hpp"

namespace cap_core {

using tcap_core::decode_integer;
using tcap_core::decode_tlv;
using tcap_core::decode_tlvs;
using tcap_core::encode_integer;
using tcap_core::encode_tlv;
using tcap_core::TagClass;
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

Tlv make_context_int(std::uint32_t tag, std::int32_t v) {
    return make_primitive(TagClass::kContext, tag, encode_integer(v));
}

Tlv make_context_bool(std::uint32_t tag, bool v) {
    // ASN.1 BOOLEAN primitive content (X.690 §8.2): FALSE=0x00, TRUE=any nonzero (0xFF used here,
    // the canonical DER value).
    return make_primitive(TagClass::kContext, tag, {v ? std::uint8_t{0xFF} : std::uint8_t{0x00}});
}

std::optional<bool> decode_bool_content(const std::vector<std::uint8_t>& value) {
    if (value.size() != 1) {
        return std::nullopt;
    }
    return value[0] != 0x00;
}

std::vector<std::uint8_t> wrap_sequence(const std::vector<Tlv>& fields) {
    std::vector<std::uint8_t> body;
    for (const auto& f : fields) {
        encode_tlv(body, f);
    }
    Tlv seq;
    seq.tag_class = TagClass::kUniversal;
    seq.constructed = true;
    seq.tag_number = UniversalTag::kSequence;
    seq.value = std::move(body);
    std::vector<std::uint8_t> out;
    encode_tlv(out, seq);
    return out;
}

// Decodes `parameter` as a single top-level universal SEQUENCE TLV and returns its member TLVs.
std::optional<std::vector<Tlv>> unwrap_sequence(const std::vector<std::uint8_t>& parameter) {
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

} // namespace

// --- InitialDpArg ---
// Real field order per TS 29.078 clause 6.1.1 InitialDPArg SEQUENCE definition (BER SEQUENCE
// elements are encoded in the type's real definition order, NOT ascending tag-number order --
// e.g. the real definition places cause[17] after eventTypeBCSM[28]).

std::vector<std::uint8_t> encode_initial_dp_arg(const InitialDpArg& arg) {
    std::vector<Tlv> fields;
    fields.push_back(make_context_int(0, arg.service_key));
    fields.push_back(make_primitive(TagClass::kContext, 2, arg.called_party_number));
    if (arg.calling_party_number.has_value()) {
        fields.push_back(make_primitive(TagClass::kContext, 3, *arg.calling_party_number));
    }
    fields.push_back(make_context_int(28, arg.event_type_bcsm));
    if (arg.cause.has_value()) {
        fields.push_back(make_primitive(TagClass::kContext, 17, encode_cause(*arg.cause)));
    }
    if (arg.imsi.has_value()) {
        fields.push_back(make_primitive(TagClass::kContext, 50, *arg.imsi));
    }
    return wrap_sequence(fields);
}

std::optional<InitialDpArg> decode_initial_dp_arg(const std::vector<std::uint8_t>& parameter) {
    const auto parts = unwrap_sequence(parameter);
    if (!parts.has_value()) {
        return std::nullopt;
    }

    InitialDpArg arg;
    const auto* service_key = find_tag(*parts, TagClass::kContext, 0);
    const auto* called = find_tag(*parts, TagClass::kContext, 2);
    if (service_key == nullptr || called == nullptr) {
        return std::nullopt;
    }
    const auto sk = decode_integer(service_key->value);
    if (!sk.has_value()) {
        return std::nullopt;
    }
    arg.service_key = *sk;
    arg.called_party_number = called->value;

    if (const auto* calling = find_tag(*parts, TagClass::kContext, 3); calling != nullptr) {
        arg.calling_party_number = calling->value;
    }

    const auto* evt = find_tag(*parts, TagClass::kContext, 28);
    if (evt == nullptr) {
        return std::nullopt;
    }
    const auto evt_val = decode_integer(evt->value);
    if (!evt_val.has_value()) {
        return std::nullopt;
    }
    arg.event_type_bcsm = *evt_val;

    if (const auto* cause = find_tag(*parts, TagClass::kContext, 17); cause != nullptr) {
        arg.cause = decode_cause(*cause);
    }
    if (const auto* imsi = find_tag(*parts, TagClass::kContext, 50); imsi != nullptr) {
        arg.imsi = imsi->value;
    }

    return arg;
}

// --- ApplyChargingArg ---
// aChBillingChargingCharacteristics [0] is a CHOICE-typed field -> EXPLICIT wrap (see
// cap_types.hpp's own header comment on this real ASN.1 rule). Only the timeDurationCharging
// variant is modeled.

std::vector<std::uint8_t> encode_apply_charging_arg(const ApplyChargingArg& arg) {
    std::vector<Tlv> tdc_fields;
    tdc_fields.push_back(make_context_int(0, arg.max_call_period_duration));
    if (arg.release_if_duration_exceeded) {
        tdc_fields.push_back(make_context_bool(1, true));
    }
    Tlv tdc_seq;
    tdc_seq.tag_class = TagClass::kContext;
    tdc_seq.constructed = true;
    tdc_seq.tag_number =
        0; // timeDurationCharging [0] SEQUENCE (CAMEL-AChBillingChargingCharacteristics)
    for (const auto& f : tdc_fields) {
        encode_tlv(tdc_seq.value, f);
    }

    std::vector<Tlv> fields;
    fields.push_back(wrap_explicit(0, tdc_seq));
    if (arg.party_to_charge.has_value()) {
        fields.push_back(wrap_explicit(2, encode_sending_side_id(*arg.party_to_charge)));
    }
    return wrap_sequence(fields);
}

std::optional<ApplyChargingArg>
decode_apply_charging_arg(const std::vector<std::uint8_t>& parameter) {
    const auto parts = unwrap_sequence(parameter);
    if (!parts.has_value()) {
        return std::nullopt;
    }

    const auto* achbcc = find_tag(*parts, TagClass::kContext, 0);
    if (achbcc == nullptr) {
        return std::nullopt;
    }
    const auto tdc = unwrap_explicit(*achbcc, 0);
    if (!tdc.has_value() || tdc->tag_class != TagClass::kContext || tdc->tag_number != 0) {
        return std::nullopt;
    }
    const auto tdc_fields = decode_tlvs(tdc->value);
    if (!tdc_fields.has_value()) {
        return std::nullopt;
    }
    const auto* max_dur = find_tag(*tdc_fields, TagClass::kContext, 0);
    if (max_dur == nullptr) {
        return std::nullopt;
    }
    const auto dur = decode_integer(max_dur->value);
    if (!dur.has_value()) {
        return std::nullopt;
    }

    ApplyChargingArg arg;
    arg.max_call_period_duration = *dur;
    if (const auto* rel = find_tag(*tdc_fields, TagClass::kContext, 1); rel != nullptr) {
        const auto b = decode_bool_content(rel->value);
        arg.release_if_duration_exceeded = b.value_or(false);
    }

    if (const auto* ptc = find_tag(*parts, TagClass::kContext, 2); ptc != nullptr) {
        const auto inner = unwrap_explicit(*ptc, 2);
        if (inner.has_value()) {
            arg.party_to_charge = decode_sending_side_id(*inner);
        }
    }

    return arg;
}

// --- ApplyChargingReportArg (= CallResult, timeDurationChargingResult variant) ---
// CallResult is an untagged CHOICE at the ApplyChargingReportArg alias point, so the parameter's
// single top-level TLV is the chosen alternative's own tag directly (context [0], constructed) --
// no extra SEQUENCE wrapper, unlike the SEQUENCE-typed ARGUMENTs above.

std::vector<std::uint8_t> encode_apply_charging_report_arg(const ApplyChargingReportArg& arg) {
    Tlv result_seq;
    result_seq.tag_class = TagClass::kContext;
    result_seq.constructed = true;
    result_seq.tag_number = 0; // timeDurationChargingResult [0] SEQUENCE (CallResult CHOICE)
    encode_tlv(result_seq.value, wrap_explicit(0, encode_receiving_side_id(arg.party_to_charge)));
    encode_tlv(
        result_seq.value,
        wrap_explicit(1, encode_time_information_no_tariff_switch(arg.elapsed_hundred_ms_units)));
    // ADR-0298: legActive [2] BOOLEAN DEFAULT TRUE. BER omits a field carrying its DEFAULT value,
    // so only `false` is ever encoded -- emitting an explicit TRUE would be legal to decode but is
    // not what a conformant encoder produces.
    if (!arg.leg_active) {
        Tlv leg_active;
        leg_active.tag_class = TagClass::kContext;
        leg_active.constructed = false;
        leg_active.tag_number = 2;
        leg_active.value = {0x00};
        encode_tlv(result_seq.value, leg_active);
    }

    std::vector<std::uint8_t> out;
    encode_tlv(out, result_seq);
    return out;
}

std::optional<ApplyChargingReportArg>
decode_apply_charging_report_arg(const std::vector<std::uint8_t>& parameter) {
    std::size_t offset = 0;
    const auto top = decode_tlv(parameter, offset);
    if (!top.has_value() || top->tag_class != TagClass::kContext || top->tag_number != 0 ||
        !top->constructed) {
        return std::nullopt;
    }
    const auto parts = decode_tlvs(top->value);
    if (!parts.has_value()) {
        return std::nullopt;
    }

    ApplyChargingReportArg arg;
    const auto* ptc = find_tag(*parts, TagClass::kContext, 0);
    const auto* ti = find_tag(*parts, TagClass::kContext, 1);
    if (ptc == nullptr || ti == nullptr) {
        return std::nullopt;
    }
    const auto ptc_inner = unwrap_explicit(*ptc, 0);
    const auto ti_inner = unwrap_explicit(*ti, 1);
    if (!ptc_inner.has_value() || !ti_inner.has_value()) {
        return std::nullopt;
    }
    const auto leg = decode_receiving_side_id(*ptc_inner);
    const auto elapsed = decode_time_information_no_tariff_switch(*ti_inner);
    if (!leg.has_value() || !elapsed.has_value()) {
        return std::nullopt;
    }
    arg.party_to_charge = *leg;
    arg.elapsed_hundred_ms_units = *elapsed;
    // ADR-0298: absent means TRUE, per the ASN.1 DEFAULT. BER encodes FALSE as a single 0x00
    // content octet and TRUE as any non-zero octet (X.690 conventionally 0xFF).
    if (const auto* la = find_tag(*parts, TagClass::kContext, 2);
        la != nullptr && !la->value.empty()) {
        arg.leg_active = la->value[0] != 0x00;
    }
    return arg;
}

// --- RequestReportBCSMEventArg / BCSMEvent ---
// legID [2] on BCSMEvent is a real CAP field whose type (LegID, imported from CS1-DataTypes) is
// not inlined in TS 29.078 itself. Modeled here as a CHOICE{sendingSideID[0], receivingSideID[1]}
// LegType -- structurally inferred from this document's own SendingSideID/ReceivingSideID
// one-arm CHOICEs (clause 5.1), NOT independently confirmed against primary CS1-DataTypes text.
// Real, disclosed gap, not a fabricated field.

std::vector<std::uint8_t>
encode_request_report_bcsm_event_arg(const RequestReportBcsmEventArg& arg) {
    std::vector<std::uint8_t> events_body;
    for (const auto& evt : arg.bcsm_events) {
        Tlv evt_seq;
        evt_seq.tag_class = TagClass::kUniversal;
        evt_seq.constructed = true;
        evt_seq.tag_number = UniversalTag::kSequence;
        encode_tlv(evt_seq.value, make_context_int(0, evt.event_type_bcsm));
        encode_tlv(evt_seq.value, make_context_int(1, evt.monitor_mode));
        if (evt.leg_id.has_value()) {
            encode_tlv(evt_seq.value, wrap_explicit(2, encode_receiving_side_id(*evt.leg_id)));
        }
        encode_tlv(events_body, evt_seq);
    }

    Tlv events_field;
    events_field.tag_class = TagClass::kContext;
    events_field.constructed = true;
    events_field.tag_number = 0; // bcsmEvents [0] SEQUENCE OF BCSMEvent (not a CHOICE -> IMPLICIT)
    events_field.value = std::move(events_body);

    return wrap_sequence({events_field});
}

std::optional<RequestReportBcsmEventArg>
decode_request_report_bcsm_event_arg(const std::vector<std::uint8_t>& parameter) {
    const auto parts = unwrap_sequence(parameter);
    if (!parts.has_value()) {
        return std::nullopt;
    }
    const auto* events_field = find_tag(*parts, TagClass::kContext, 0);
    if (events_field == nullptr) {
        return std::nullopt;
    }
    const auto event_tlvs = decode_tlvs(events_field->value);
    if (!event_tlvs.has_value()) {
        return std::nullopt;
    }

    RequestReportBcsmEventArg arg;
    for (const auto& evt_seq : *event_tlvs) {
        if (evt_seq.tag_class != TagClass::kUniversal ||
            evt_seq.tag_number != UniversalTag::kSequence) {
            return std::nullopt;
        }
        const auto evt_parts = decode_tlvs(evt_seq.value);
        if (!evt_parts.has_value()) {
            return std::nullopt;
        }
        const auto* et = find_tag(*evt_parts, TagClass::kContext, 0);
        const auto* mm = find_tag(*evt_parts, TagClass::kContext, 1);
        if (et == nullptr || mm == nullptr) {
            return std::nullopt;
        }
        const auto et_val = decode_integer(et->value);
        const auto mm_val = decode_integer(mm->value);
        if (!et_val.has_value() || !mm_val.has_value()) {
            return std::nullopt;
        }
        BcsmEvent evt;
        evt.event_type_bcsm = *et_val;
        evt.monitor_mode = *mm_val;
        if (const auto* lid = find_tag(*evt_parts, TagClass::kContext, 2); lid != nullptr) {
            const auto inner = unwrap_explicit(*lid, 2);
            if (inner.has_value()) {
                evt.leg_id = decode_receiving_side_id(*inner);
            }
        }
        arg.bcsm_events.push_back(evt);
    }
    return arg;
}

// --- EventReportBCSMArg ---

std::vector<std::uint8_t> encode_event_report_bcsm_arg(const EventReportBcsmArg& arg) {
    std::vector<Tlv> fields;
    fields.push_back(make_context_int(0, arg.event_type_bcsm));
    if (arg.leg_id.has_value()) {
        fields.push_back(wrap_explicit(3, encode_receiving_side_id(*arg.leg_id)));
    }
    return wrap_sequence(fields);
}

std::optional<EventReportBcsmArg>
decode_event_report_bcsm_arg(const std::vector<std::uint8_t>& parameter) {
    const auto parts = unwrap_sequence(parameter);
    if (!parts.has_value()) {
        return std::nullopt;
    }
    const auto* et = find_tag(*parts, TagClass::kContext, 0);
    if (et == nullptr) {
        return std::nullopt;
    }
    const auto et_val = decode_integer(et->value);
    if (!et_val.has_value()) {
        return std::nullopt;
    }
    EventReportBcsmArg arg;
    arg.event_type_bcsm = *et_val;
    if (const auto* lid = find_tag(*parts, TagClass::kContext, 3); lid != nullptr) {
        const auto inner = unwrap_explicit(*lid, 3);
        if (inner.has_value()) {
            arg.leg_id = decode_receiving_side_id(*inner);
        }
    }
    return arg;
}

// --- ReleaseCallArg (allCallSegments variant: a bare Cause) ---

std::vector<std::uint8_t> encode_release_call_arg(const std::vector<std::uint8_t>& cause_octets) {
    Tlv tlv;
    tlv.tag_class = TagClass::kUniversal;
    tlv.constructed = false;
    tlv.tag_number = UniversalTag::kOctetString;
    tlv.value = encode_cause(cause_octets);
    std::vector<std::uint8_t> out;
    encode_tlv(out, tlv);
    return out;
}

std::optional<std::vector<std::uint8_t>>
decode_release_call_arg(const std::vector<std::uint8_t>& parameter) {
    std::size_t offset = 0;
    const auto tlv = decode_tlv(parameter, offset);
    if (!tlv.has_value() || tlv->tag_class != TagClass::kUniversal ||
        tlv->tag_number != UniversalTag::kOctetString) {
        return std::nullopt;
    }
    return decode_cause(*tlv);
}

} // namespace cap_core
