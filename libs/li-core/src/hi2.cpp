#include "li_core/hi2.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <tl/expected.hpp>
#include <type_traits>
#include <vector>

#include "li_core/xiri.hpp"

// asn1c-generated codecs (li_generated). Two ASN.1 modules meet here and nowhere else in the
// repository: ETSI TS 102 232-1's PS-PDU envelope (the LI-PS-PDU-3GPP-Subset of ADR-0373) and
// TS 33.128's IRIPayload. They stay decoupled on the wire -- the 33.128 payload goes into the
// envelope as opaque octets -- and they are decoupled here too: the envelope side never names a
// TS33128Payloads type. Hidden-visibility symbols, included only from this .cpp (ADR-0364).
extern "C" {
#include <GeneralizedTime.h>
#include <INTEGER.h>
#include <IRIContents.h>
#include <IRIEvent.h>
#include <IRIPayload.h>
#include <IRITargetIdentifier.h>
#include <MediatedFromIndicator.h>
#include <MicroSecondTimeStamp.h>
#include <OBJECT_IDENTIFIER.h>
#include <OCTET_STRING.h>
#include <PS-PDU.h>
#include <PSIRIPayload.h>
#include <RELATIVE-OID.h>
#include <TargetIdentifier.h>
#include <XIRIEvent.h>
#include <XIRIPayload.h>
#include <asn_codecs.h>
#include <ber_decoder.h>
#include <constr_TYPE.h>
#include <der_encoder.h>
}

namespace li_core::hi2 {

namespace {

// asn1c allocates every node with calloc and each descriptor's free walks the whole tree, so one
// deleter on the root covers everything hung off it.
template <typename T, asn_TYPE_descriptor_t* Descriptor> struct AsnDeleter {
    void operator()(T* p) const { ASN_STRUCT_FREE(*Descriptor, p); }
};
template <typename T, asn_TYPE_descriptor_t* Descriptor>
using AsnPtr = std::unique_ptr<T, AsnDeleter<T, Descriptor>>;

template <typename T> T* alloc() {
    return static_cast<T*>(calloc(1, sizeof(T)));
}

int append_bytes(const void* data, std::size_t size, void* key) {
    auto* sink = static_cast<std::vector<std::uint8_t>*>(key);
    const auto* octets = static_cast<const std::uint8_t*>(data);
    sink->insert(sink->end(), octets, octets + size);
    return 0;
}

tl::expected<std::vector<std::uint8_t>, std::string>
der_encode_to_vector(asn_TYPE_descriptor_t& descriptor, void* structure) {
    std::vector<std::uint8_t> out;
    const asn_enc_rval_t rv = der_encode(&descriptor, structure, append_bytes, &out);
    if (rv.encoded < 0 || static_cast<std::size_t>(rv.encoded) != out.size()) {
        return tl::make_unexpected(std::string("DER encode of ") + descriptor.name + " failed at " +
                                   (rv.failed_type != nullptr ? rv.failed_type->name : "?"));
    }
    return out;
}

std::string octets_to_std(const OCTET_STRING_t& octets) {
    if (octets.buf == nullptr || octets.size <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(octets.buf),
                       static_cast<std::size_t>(octets.size));
}

// ETSI TS 102 232-1 clause 5.2.6: a GeneralizedTime in UTC. asn_time2GT writes the
// "YYYYMMDDhhmmssZ" form asn1c's own decoder accepts.
bool set_generalized_time(GeneralizedTime_t& dst, std::uint64_t seconds) {
    const auto epoch = static_cast<time_t>(seconds);
    struct tm utc {};
    if (gmtime_r(&epoch, &utc) == nullptr) {
        return false;
    }
    return asn_time2GT(&dst, &utc, 1) != nullptr;
}

std::optional<std::uint64_t> read_generalized_time(const GeneralizedTime_t& src) {
    const time_t t = asn_GT2time(&src, nullptr, 0);
    if (t == static_cast<time_t>(-1)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(t);
}

// TS 33.128 clause 5.5.5 provenance -> TS33128Payloads TargetIdentifierProvenance. The ASN.1
// ENUMERATED values are the ones the enum class carries, so this is a cast with a name.
long provenance_value(Provenance p) {
    return static_cast<long>(p);
}

// Table 6.2.1.2-2 (X1) identifier kinds -> the TargetIdentifier CHOICE of TS33128Payloads.
// Two X1 kinds have no alternative in that CHOICE and are reported, not guessed: `suci` (the
// CHOICE has no SUCI -- de-concealment happens at the UDM POI, TS 33.127 6.2.2.3) and `Other`
// (an element this codec does not model).
tl::expected<void, std::string> fill_target_identifier(TargetIdentifier_t& out,
                                                       const x1::TargetIdentifier& src) {
    const auto set_numeric = [&](void* dst) {
        return OCTET_STRING_fromBuf(static_cast<OCTET_STRING_t*>(dst),
                                    src.value.data(),
                                    static_cast<int>(src.value.size())) == 0;
    };
    const auto parse_ipv4 = [&](OCTET_STRING_t& dst) {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        if (sscanf(src.value.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 ||
            c > 255 || d > 255) {
            return false;
        }
        const std::uint8_t octets[4] = {static_cast<std::uint8_t>(a),
                                        static_cast<std::uint8_t>(b),
                                        static_cast<std::uint8_t>(c),
                                        static_cast<std::uint8_t>(d)};
        return OCTET_STRING_fromBuf(&dst, reinterpret_cast<const char*>(octets), 4) == 0;
    };
    const auto parse_ipv6 = [&](OCTET_STRING_t& dst) {
        std::uint8_t octets[16]{};
        if (inet_pton(AF_INET6, src.value.c_str(), octets) != 1) {
            return false;
        }
        return OCTET_STRING_fromBuf(&dst, reinterpret_cast<const char*>(octets), 16) == 0;
    };

    switch (src.kind) {
        case x1::TargetIdentifierKind::SupiImsi:
            out.present = TargetIdentifier_PR_sUPI;
            out.choice.sUPI.present = SUPI_PR_iMSI;
            if (!set_numeric(&out.choice.sUPI.choice.iMSI)) {
                return tl::make_unexpected(std::string("SUPI/IMSI allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::SupiNai:
            out.present = TargetIdentifier_PR_sUPI;
            out.choice.sUPI.present = SUPI_PR_nAI;
            if (!set_numeric(&out.choice.sUPI.choice.nAI)) {
                return tl::make_unexpected(std::string("SUPI/NAI allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::PeiImei:
            out.present = TargetIdentifier_PR_pEI;
            out.choice.pEI.present = PEI_PR_iMEI;
            if (!set_numeric(&out.choice.pEI.choice.iMEI)) {
                return tl::make_unexpected(std::string("PEI/IMEI allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::PeiImeisv:
            out.present = TargetIdentifier_PR_pEI;
            out.choice.pEI.present = PEI_PR_iMEISV;
            if (!set_numeric(&out.choice.pEI.choice.iMEISV)) {
                return tl::make_unexpected(std::string("PEI/IMEISV allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::GpsiMsisdn:
            out.present = TargetIdentifier_PR_gPSI;
            out.choice.gPSI.present = GPSI_PR_mSISDN;
            if (!set_numeric(&out.choice.gPSI.choice.mSISDN)) {
                return tl::make_unexpected(std::string("GPSI/MSISDN allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::GpsiNai:
            out.present = TargetIdentifier_PR_gPSI;
            out.choice.gPSI.present = GPSI_PR_nAI;
            if (!set_numeric(&out.choice.gPSI.choice.nAI)) {
                return tl::make_unexpected(std::string("GPSI/NAI allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::Imsi:
            out.present = TargetIdentifier_PR_iMSI;
            if (!set_numeric(&out.choice.iMSI)) {
                return tl::make_unexpected(std::string("IMSI allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::Imei:
            out.present = TargetIdentifier_PR_iMEI;
            if (!set_numeric(&out.choice.iMEI)) {
                return tl::make_unexpected(std::string("IMEI allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::Msisdn:
            // X1's element is <e164Number>; the CHOICE alternative of the same name is [13].
            out.present = TargetIdentifier_PR_e164Number;
            if (!set_numeric(&out.choice.e164Number)) {
                return tl::make_unexpected(std::string("E164Number allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::Nai:
            out.present = TargetIdentifier_PR_nAI;
            if (!set_numeric(&out.choice.nAI)) {
                return tl::make_unexpected(std::string("NAI allocation failed"));
            }
            return {};
        case x1::TargetIdentifierKind::Ipv4Address:
            out.present = TargetIdentifier_PR_iPv4Address;
            if (!parse_ipv4(out.choice.iPv4Address)) {
                return tl::make_unexpected(std::string("IPv4 target identifier '") + src.value +
                                           "' is not a dotted quad");
            }
            return {};
        case x1::TargetIdentifierKind::Ipv6Address:
            out.present = TargetIdentifier_PR_iPv6Address;
            if (!parse_ipv6(out.choice.iPv6Address)) {
                return tl::make_unexpected(std::string("IPv6 target identifier '") + src.value +
                                           "' is not an IPv6 address");
            }
            return {};
        case x1::TargetIdentifierKind::Suci:
        case x1::TargetIdentifierKind::Other:
            break;
    }
    return tl::make_unexpected(std::string("target identifier <") + src.element +
                               "> has no alternative in TS33128Payloads.TargetIdentifier");
}

} // namespace

tl::expected<MediationResult, std::string>
mediate_xiri(std::span<const std::uint8_t> xiri_payload,
             const std::vector<IriTargetIdentifier>& target_identifiers) {
    // 1. Decode the xIRI the POI sent.
    XIRIPayload_t* raw_xiri = nullptr;
    const asn_dec_rval_t dr = ber_decode(nullptr,
                                         &asn_DEF_XIRIPayload,
                                         reinterpret_cast<void**>(&raw_xiri),
                                         xiri_payload.data(),
                                         xiri_payload.size());
    AsnPtr<XIRIPayload_t, &asn_DEF_XIRIPayload> xiri(raw_xiri);
    if (dr.code != RC_OK) {
        return tl::make_unexpected(std::string(dr.code == RC_WMORE ? "truncated" : "malformed") +
                                   " XIRIPayload on LI_X2");
    }
    if (dr.consumed != xiri_payload.size()) {
        return tl::make_unexpected(std::string("trailing bytes after the XIRIPayload"));
    }
    if (xiri->event.present == XIRIEvent_PR_NOTHING) {
        return tl::make_unexpected(std::string("XIRIPayload.event carries no known alternative"));
    }

    // 2. "MDF2 shall choose the same choice for the IRIPayload.event that was received in the
    // xIRIPayload.event" (table 5.5.2-2). XIRIEvent and IRIEvent share 195 alternatives with
    // identical tag, name and type, so the alternative's own TLV transfers verbatim: re-encode
    // the CHOICE, decode it against IRIEvent. The three xIRI-only alternatives (n9HRPDUSessionInfo
    // [100], s8HRBearerInfo [101], iPIRIPacketReport [161]) have no IRIEvent alternative, and
    // because TS33128Payloads is EXTENSIBILITY IMPLIED asn1c *accepts* their tag as an unknown
    // extension and returns RC_OK with present == IRIEvent_PR_NOTHING -- so the presence check
    // below, not the return code, is what rejects them.
    auto event_tlv = der_encode_to_vector(asn_DEF_XIRIEvent, &xiri->event);
    if (!event_tlv) {
        return tl::make_unexpected(event_tlv.error());
    }
    IRIEvent_t* raw_event = nullptr;
    const asn_dec_rval_t er = ber_decode(nullptr,
                                         &asn_DEF_IRIEvent,
                                         reinterpret_cast<void**>(&raw_event),
                                         event_tlv->data(),
                                         event_tlv->size());
    AsnPtr<IRIEvent_t, &asn_DEF_IRIEvent> event(raw_event);
    if (er.code != RC_OK || !event || event->present == IRIEvent_PR_NOTHING) {
        return tl::make_unexpected(
            std::string("XIRIEvent alternative ") +
            std::to_string(static_cast<int>(xiri->event.present)) +
            " has no IRIEvent counterpart (TS 33.128 Annex A); it cannot be mediated to LI_HI2");
    }

    // 3. Assemble the IRIPayload (table 5.5.2-2).
    AsnPtr<IRIPayload_t, &asn_DEF_IRIPayload> iri(alloc<IRIPayload_t>());
    if (!iri) {
        return tl::make_unexpected(std::string("allocation failed"));
    }
    if (RELATIVE_OID_set_arcs(
            &iri->iRIPayloadOID, kIriPayloadOidArcs, std::size(kIriPayloadOidArcs)) != 0) {
        return tl::make_unexpected(std::string("iRIPayloadOID allocation failed"));
    }
    iri->event = *event; // move the decoded tree in; the source struct is zeroed so
    std::memset(event.get(), 0, sizeof(IRIEvent_t)); // its deleter does not free what we now own.

    MediationResult result;

    // mediatedFromIndicator: "shall be present if ... the release and version of the
    // xIRIPayload.relativeOID is different from the release and version of the
    // IRIPayload.relativeOID" (table 5.5.2-2).
    asn_oid_arc_t received[16];
    const ssize_t received_count =
        RELATIVE_OID_get_arcs(&xiri->xIRIPayloadOID, received, std::size(received));
    if (received_count < 0) {
        return tl::make_unexpected(std::string("xIRIPayloadOID unreadable"));
    }
    // Arcs are {threeGPP(4) ts33128(19) release version payloadKind}: compare release + version.
    const bool same_release_and_version = received_count >= 4 &&
                                          received[2] == kIriPayloadOidArcs[2] &&
                                          received[3] == kIriPayloadOidArcs[3];
    if (!same_release_and_version) {
        auto* indicator = alloc<MediatedFromIndicator_t>();
        if (indicator == nullptr) {
            return tl::make_unexpected(std::string("mediatedFromIndicator allocation failed"));
        }
        indicator->present = MediatedFromIndicator_PR_xIRIRelativeOID;
        if (RELATIVE_OID_set_arcs(&indicator->choice.xIRIRelativeOID,
                                  received,
                                  static_cast<size_t>(received_count)) != 0) {
            ASN_STRUCT_FREE(asn_DEF_MediatedFromIndicator, indicator);
            return tl::make_unexpected(std::string("mediatedFromIndicator OID allocation failed"));
        }
        iri->mediatedFromIndicator = indicator;
        result.mediated_from_indicator = true;
    }

    // targetIdentifiers, each with its clause-5.5.5 provenance.
    if (!target_identifiers.empty()) {
        using TargetIdentifierList =
            std::remove_pointer_t<decltype(IRIPayload_t::targetIdentifiers)>;
        auto* list = alloc<TargetIdentifierList>();
        if (list == nullptr) {
            return tl::make_unexpected(std::string("targetIdentifiers allocation failed"));
        }
        iri->targetIdentifiers = list;
        for (const auto& source : target_identifiers) {
            auto* entry = alloc<IRITargetIdentifier_t>();
            if (entry == nullptr) {
                return tl::make_unexpected(std::string("IRITargetIdentifier allocation failed"));
            }
            auto filled = fill_target_identifier(entry->identifier, source.identifier);
            if (!filled) {
                ASN_STRUCT_FREE(asn_DEF_IRITargetIdentifier, entry);
                return tl::make_unexpected(filled.error());
            }
            auto* provenance = alloc<TargetIdentifierProvenance_t>();
            if (provenance == nullptr) {
                ASN_STRUCT_FREE(asn_DEF_IRITargetIdentifier, entry);
                return tl::make_unexpected(std::string("provenance allocation failed"));
            }
            *provenance = provenance_value(source.provenance);
            entry->provenance = provenance;
            if (ASN_SEQUENCE_ADD(&list->list, entry) != 0) {
                ASN_STRUCT_FREE(asn_DEF_IRITargetIdentifier, entry);
                return tl::make_unexpected(std::string("targetIdentifiers append failed"));
            }
        }
    }

    auto encoded = der_encode_to_vector(asn_DEF_IRIPayload, iri.get());
    if (!encoded) {
        return tl::make_unexpected(encoded.error());
    }
    result.iri_payload = std::move(*encoded);
    return result;
}

tl::expected<std::vector<std::uint8_t>, std::string> encode_iri_message(const IriMessage& message) {
    if (message.liid.empty() || message.liid.size() > 25) {
        return tl::make_unexpected(
            std::string("LIID must be 1..25 octets (TS 103 280 LIID), got ") +
            std::to_string(message.liid.size()));
    }
    const auto& cid = message.communication_identifier;
    if (cid.operator_identifier.empty() || cid.operator_identifier.size() > 16) {
        return tl::make_unexpected(std::string("operatorIdentifier must be 1..16 octets"));
    }
    if (cid.network_element_identifier &&
        (cid.network_element_identifier->empty() || cid.network_element_identifier->size() > 16)) {
        return tl::make_unexpected(std::string("networkElementIdentifier must be 1..16 octets"));
    }
    if (cid.delivery_country_code && cid.delivery_country_code->size() != 2) {
        return tl::make_unexpected(std::string("deliveryCountryCode must be 2 characters"));
    }
    if (message.authorization_country_code && message.authorization_country_code->size() != 2) {
        return tl::make_unexpected(std::string("authorizationCountryCode must be 2 characters"));
    }
    if (message.interception_point_id &&
        (message.interception_point_id->empty() || message.interception_point_id->size() > 8)) {
        return tl::make_unexpected(std::string("interceptionPointID must be 1..8 characters"));
    }
    if (message.iri_payload.empty()) {
        return tl::make_unexpected(std::string("IRIContents.threeGPP33128DefinedIRI is empty"));
    }

    AsnPtr<PS_PDU_t, &asn_DEF_PS_PDU> pdu(alloc<PS_PDU_t>());
    if (!pdu) {
        return tl::make_unexpected(std::string("allocation failed"));
    }
    PSHeader_t& header = pdu->pSHeader;
    if (OBJECT_IDENTIFIER_set_arcs(
            &header.li_psDomainId, kLiPsDomainIdArcs, std::size(kLiPsDomainIdArcs)) != 0) {
        return tl::make_unexpected(std::string("li-psDomainId allocation failed"));
    }
    if (OCTET_STRING_fromBuf(&header.lawfulInterceptionIdentifier,
                             message.liid.data(),
                             static_cast<int>(message.liid.size())) != 0) {
        return tl::make_unexpected(std::string("LIID allocation failed"));
    }
    if (OCTET_STRING_fromBuf(&header.communicationIdentifier.networkIdentifier.operatorIdentifier,
                             cid.operator_identifier.data(),
                             static_cast<int>(cid.operator_identifier.size())) != 0) {
        return tl::make_unexpected(std::string("operatorIdentifier allocation failed"));
    }
    if (cid.network_element_identifier) {
        auto* ne = alloc<OCTET_STRING_t>();
        if (ne == nullptr ||
            OCTET_STRING_fromBuf(ne,
                                 cid.network_element_identifier->data(),
                                 static_cast<int>(cid.network_element_identifier->size())) != 0) {
            return tl::make_unexpected(std::string("networkElementIdentifier allocation failed"));
        }
        header.communicationIdentifier.networkIdentifier.networkElementIdentifier = ne;
    }
    if (cid.communication_identity_number) {
        auto* cin = alloc<unsigned long>();
        if (cin == nullptr) {
            return tl::make_unexpected(
                std::string("communicationIdentityNumber allocation failed"));
        }
        *cin = static_cast<unsigned long>(*cid.communication_identity_number);
        header.communicationIdentifier.communicationIdentityNumber = cin;
    }
    if (cid.delivery_country_code) {
        auto* dcc = alloc<PrintableString_t>();
        if (dcc == nullptr ||
            OCTET_STRING_fromBuf(dcc,
                                 cid.delivery_country_code->data(),
                                 static_cast<int>(cid.delivery_country_code->size())) != 0) {
            return tl::make_unexpected(std::string("deliveryCountryCode allocation failed"));
        }
        header.communicationIdentifier.deliveryCountryCode = dcc;
    }
    header.sequenceNumber = message.sequence_number;
    if (message.authorization_country_code) {
        auto* acc = alloc<PrintableString_t>();
        if (acc == nullptr ||
            OCTET_STRING_fromBuf(acc,
                                 message.authorization_country_code->data(),
                                 static_cast<int>(message.authorization_country_code->size())) !=
                0) {
            return tl::make_unexpected(std::string("authorizationCountryCode allocation failed"));
        }
        header.authorizationCountryCode = acc;
    }
    if (message.interception_point_id) {
        auto* ipid = alloc<PrintableString_t>();
        if (ipid == nullptr ||
            OCTET_STRING_fromBuf(ipid,
                                 message.interception_point_id->data(),
                                 static_cast<int>(message.interception_point_id->size())) != 0) {
            return tl::make_unexpected(std::string("interceptionPointID allocation failed"));
        }
        header.interceptionPointID = ipid;
    }
    if (message.extended_interception_point_id) {
        auto* ext = alloc<OCTET_STRING_t>();
        if (ext == nullptr ||
            OCTET_STRING_fromBuf(
                ext,
                message.extended_interception_point_id->data(),
                static_cast<int>(message.extended_interception_point_id->size())) != 0) {
            return tl::make_unexpected(
                std::string("extendedInterceptionPointID allocation failed"));
        }
        header.extendedInterceptionPointID = ext;
    }
    if (message.network_function_identifier) {
        auto* nfid = alloc<OCTET_STRING_t>();
        if (nfid == nullptr ||
            OCTET_STRING_fromBuf(nfid,
                                 message.network_function_identifier->data(),
                                 static_cast<int>(message.network_function_identifier->size())) !=
                0) {
            return tl::make_unexpected(std::string("networkFunctionIdentifier allocation failed"));
        }
        header.networkFunctionIdentifier = nfid;
    }
    if (message.timestamp) {
        auto* gt = alloc<GeneralizedTime_t>();
        if (gt == nullptr || !set_generalized_time(*gt, message.timestamp->seconds)) {
            return tl::make_unexpected(std::string("PSHeader timeStamp allocation failed"));
        }
        header.timeStamp = gt;
        auto* micro = alloc<MicroSecondTimeStamp_t>();
        if (micro == nullptr ||
            asn_umax2INTEGER(&micro->seconds, static_cast<uintmax_t>(message.timestamp->seconds)) !=
                0) {
            return tl::make_unexpected(std::string("microSecondTimeStamp allocation failed"));
        }
        micro->microSeconds = static_cast<long>(message.timestamp->microseconds);
        header.microSecondTimeStamp = micro;
    }
    // Table 5.5.1-1: "the timestamp qualifier shall be present" on IRI messages.
    auto* qualifier = alloc<TimeStampQualifier_t>();
    if (qualifier == nullptr) {
        return tl::make_unexpected(std::string("timeStampQualifier allocation failed"));
    }
    *qualifier = static_cast<long>(message.timestamp_qualifier);
    header.timeStampQualifier = qualifier;

    // One PSIRIPayload in the sequence (no aggregation, see the header note).
    pdu->payload.present = Payload_PR_iRIPayloadSequence;
    auto* iri_payload = alloc<PSIRIPayload_t>();
    if (iri_payload == nullptr) {
        return tl::make_unexpected(std::string("PSIRIPayload allocation failed"));
    }
    auto* iri_type = alloc<IRIType_t>();
    if (iri_type == nullptr) {
        ASN_STRUCT_FREE(asn_DEF_PSIRIPayload, iri_payload);
        return tl::make_unexpected(std::string("IRI-Type allocation failed"));
    }
    *iri_type = static_cast<long>(message.iri_type);
    iri_payload->iRIType = iri_type;
    iri_payload->iRIContents.present = IRIContents_PR_threeGPP33128DefinedIRI;
    if (OCTET_STRING_fromBuf(&iri_payload->iRIContents.choice.threeGPP33128DefinedIRI,
                             reinterpret_cast<const char*>(message.iri_payload.data()),
                             static_cast<int>(message.iri_payload.size())) != 0) {
        ASN_STRUCT_FREE(asn_DEF_PSIRIPayload, iri_payload);
        return tl::make_unexpected(std::string("threeGPP33128DefinedIRI allocation failed"));
    }
    if (ASN_SEQUENCE_ADD(&pdu->payload.choice.iRIPayloadSequence.list, iri_payload) != 0) {
        ASN_STRUCT_FREE(asn_DEF_PSIRIPayload, iri_payload);
        return tl::make_unexpected(std::string("iRIPayloadSequence append failed"));
    }

    return der_encode_to_vector(asn_DEF_PS_PDU, pdu.get());
}

tl::expected<IriMessage, std::string> decode_iri_message(std::span<const std::uint8_t> bytes) {
    PS_PDU_t* raw = nullptr;
    const asn_dec_rval_t dr = ber_decode(
        nullptr, &asn_DEF_PS_PDU, reinterpret_cast<void**>(&raw), bytes.data(), bytes.size());
    AsnPtr<PS_PDU_t, &asn_DEF_PS_PDU> pdu(raw);
    if (dr.code != RC_OK) {
        return tl::make_unexpected(std::string(dr.code == RC_WMORE ? "truncated" : "malformed") +
                                   " PS-PDU");
    }
    if (dr.consumed != bytes.size()) {
        return tl::make_unexpected(std::string("trailing bytes after the PS-PDU"));
    }

    const PSHeader_t& header = pdu->pSHeader;
    asn_oid_arc_t domain[16];
    const ssize_t domain_count =
        OBJECT_IDENTIFIER_get_arcs(&header.li_psDomainId, domain, std::size(domain));
    if (domain_count != static_cast<ssize_t>(std::size(kLiPsDomainIdArcs)) ||
        !std::equal(std::begin(kLiPsDomainIdArcs), std::end(kLiPsDomainIdArcs), domain)) {
        return tl::make_unexpected(std::string("PSHeader.li-psDomainId is not the TS 102 232-1 "
                                               "version43 domain id"));
    }

    IriMessage out;
    out.liid = octets_to_std(header.lawfulInterceptionIdentifier);
    out.communication_identifier.operator_identifier =
        octets_to_std(header.communicationIdentifier.networkIdentifier.operatorIdentifier);
    if (header.communicationIdentifier.networkIdentifier.networkElementIdentifier != nullptr) {
        out.communication_identifier.network_element_identifier = octets_to_std(
            *header.communicationIdentifier.networkIdentifier.networkElementIdentifier);
    }
    if (header.communicationIdentifier.communicationIdentityNumber != nullptr) {
        out.communication_identifier.communication_identity_number =
            static_cast<std::uint32_t>(*header.communicationIdentifier.communicationIdentityNumber);
    }
    if (header.communicationIdentifier.deliveryCountryCode != nullptr) {
        out.communication_identifier.delivery_country_code =
            octets_to_std(*header.communicationIdentifier.deliveryCountryCode);
    }
    out.sequence_number = static_cast<std::uint32_t>(header.sequenceNumber);
    if (header.authorizationCountryCode != nullptr) {
        out.authorization_country_code = octets_to_std(*header.authorizationCountryCode);
    }
    if (header.interceptionPointID != nullptr) {
        out.interception_point_id = octets_to_std(*header.interceptionPointID);
    }
    if (header.extendedInterceptionPointID != nullptr) {
        out.extended_interception_point_id = octets_to_std(*header.extendedInterceptionPointID);
    }
    if (header.networkFunctionIdentifier != nullptr) {
        out.network_function_identifier = octets_to_std(*header.networkFunctionIdentifier);
    }
    if (header.timeStamp != nullptr) {
        const auto seconds = read_generalized_time(*header.timeStamp);
        if (!seconds) {
            return tl::make_unexpected(std::string("PSHeader.timeStamp is not a readable "
                                                   "GeneralizedTime"));
        }
        Timestamp ts{*seconds, 0};
        if (header.microSecondTimeStamp != nullptr) {
            ts.microseconds = static_cast<std::uint32_t>(header.microSecondTimeStamp->microSeconds);
        }
        out.timestamp = ts;
    }
    if (header.timeStampQualifier != nullptr) {
        out.timestamp_qualifier = static_cast<TimestampQualifier>(*header.timeStampQualifier);
    }

    if (pdu->payload.present != Payload_PR_iRIPayloadSequence) {
        return tl::make_unexpected(std::string("PS-PDU payload is not an iRIPayloadSequence"));
    }
    const auto& list = pdu->payload.choice.iRIPayloadSequence.list;
    if (list.count != 1) {
        return tl::make_unexpected(std::string("expected exactly one PSIRIPayload, got ") +
                                   std::to_string(list.count));
    }
    const PSIRIPayload_t& iri = *list.array[0];
    if (iri.iRIType != nullptr) {
        out.iri_type = static_cast<IriType>(*iri.iRIType);
    }
    if (iri.iRIContents.present != IRIContents_PR_threeGPP33128DefinedIRI) {
        return tl::make_unexpected(
            std::string("IRIContents is not the threeGPP33128DefinedIRI alternative"));
    }
    const OCTET_STRING_t& payload = iri.iRIContents.choice.threeGPP33128DefinedIRI;
    out.iri_payload.assign(payload.buf, payload.buf + payload.size);
    return out;
}

tl::expected<std::vector<std::uint8_t>, std::string>
mediate_x2_pdu(const Pdu& pdu, const MediationContext& context) {
    if (pdu.type != PduType::X2) {
        return tl::make_unexpected(std::string("LI_HI2 mediation takes an X2 PDU, got PDU Type ") +
                                   std::to_string(static_cast<int>(pdu.type)));
    }
    if (pdu.payload_format != PayloadFormat::Tgpp33128Payload) {
        return tl::make_unexpected(
            std::string("LI_X2 Payload Format is ") +
            std::to_string(static_cast<int>(pdu.payload_format)) +
            "; only Format 2 (a BER TS33128Payloads structure) mediates to an IRIPayload");
    }

    // Clause 5.5.5: every identifier from the Matched Target Identifier attribute is "matchedOn",
    // every one from Other Target Identifier is "other". Both attributes may repeat (5.3.18/19).
    std::vector<IriTargetIdentifier> identifiers = context.target_identifiers;
    const auto collect = [&](AttributeType type,
                             Provenance provenance) -> tl::expected<void, std::string> {
        for (const auto& fragment : text_attributes(pdu, type)) {
            auto parsed = x1::parse_target_identifier_fragment(fragment);
            if (!parsed) {
                return tl::make_unexpected(parsed.error());
            }
            identifiers.push_back(IriTargetIdentifier{*parsed, provenance});
        }
        return {};
    };
    if (auto matched = collect(AttributeType::MatchedTargetIdentifier, Provenance::MatchedOn);
        !matched) {
        return tl::make_unexpected(matched.error());
    }
    if (auto other = collect(AttributeType::OtherTargetIdentifier, Provenance::Other); !other) {
        return tl::make_unexpected(other.error());
    }

    auto mediated = mediate_xiri(pdu.payload, identifiers);
    if (!mediated) {
        return tl::make_unexpected(mediated.error());
    }

    IriMessage message;
    message.liid = context.liid;
    message.communication_identifier = context.communication_identifier;
    message.sequence_number = context.sequence_number;
    message.authorization_country_code = context.authorization_country_code;
    message.interception_point_id = context.interception_point_id;
    message.iri_type = context.iri_type;
    message.iri_payload = std::move(mediated->iri_payload);
    // Table 5.5.1-1 mappings that come from the PDU itself.
    message.network_function_identifier = text_attribute(pdu, AttributeType::NetworkFunctionId);
    message.extended_interception_point_id =
        text_attribute(pdu, AttributeType::InterceptionPointId);
    if (const auto observed = timestamp(pdu)) {
        message.timestamp = Timestamp{observed->seconds, observed->nanoseconds / 1000};
    }
    // Table 5.5.2-1: "if the timestamp field is set, the timestamp qualifier ... shall be present
    // and set to timeOfInterception(1)" -- the PDU timestamp is the time the POI observed it.
    message.timestamp_qualifier = TimestampQualifier::TimeOfInterception;

    return encode_iri_message(message);
}

} // namespace li_core::hi2
