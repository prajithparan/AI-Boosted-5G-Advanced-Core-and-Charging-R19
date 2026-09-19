#include "li_core/xiri.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <sys/types.h>
#include <tl/expected.hpp>
#include <type_traits>
#include <variant>
#include <vector>

// asn1c-generated TS33128Payloads codec (li_generated). Hidden-visibility symbols; included only
// here, never from a public header -- see xiri.hpp and ADR-0364.
extern "C" {
#include <AMFDeregistration.h>
#include <AMFLocationUpdate.h>
#include <AMFRegistration.h>
#include <AMFStartOfInterceptionWithRegisteredUE.h>
#include <BIT_STRING.h>
#include <ECGI.h>
#include <EUTRALocation.h>
#include <Location.h>
#include <LocationInfo.h>
#include <NCGI.h>
#include <NRLocation.h>
#include <NumericString.h>
#include <PLMNID.h>
#include <OBJECT_IDENTIFIER.h>
#include <OCTET_STRING.h>
#include <RELATIVE-OID.h>
#include <TAI.h>
#include <UserLocation.h>
#include <XIRIPayload.h>
#include <asn_codecs.h>
#include <ber_decoder.h>
#include <constr_TYPE.h>
#include <der_encoder.h>
}

namespace li_core::xiri {

namespace {

struct PayloadDeleter {
    void operator()(XIRIPayload_t* payload) const { ASN_STRUCT_FREE(asn_DEF_XIRIPayload, payload); }
};
using PayloadPtr = std::unique_ptr<XIRIPayload_t, PayloadDeleter>;

// asn1c allocates with calloc; the descriptor's free walks the whole tree, so one deleter covers
// everything hung off the top-level struct.
PayloadPtr new_payload() {
    auto* raw = static_cast<XIRIPayload_t*>(calloc(1, sizeof(XIRIPayload_t)));
    return PayloadPtr(raw);
}

int set_numeric_string(NumericString_t& dst, const std::string& digits) {
    return OCTET_STRING_fromBuf(&dst, digits.data(), static_cast<int>(digits.size()));
}

std::string octet_string_to_std(const OCTET_STRING_t& octets) {
    return std::string(reinterpret_cast<const char*>(octets.buf),
                       static_cast<std::size_t>(octets.size));
}

// SUPI ::= CHOICE { iMSI [1] IMSI, nAI [2] NAI }. Shared by every AMF xIRI that carries a SUPI.
tl::expected<void, std::string> fill_supi(SUPI_t& out, const Supi& supi) {
    if (const auto* imsi = std::get_if<Imsi>(&supi)) {
        if (imsi->digits.size() < 6 || imsi->digits.size() > 15) {
            return tl::unexpected(
                "IMSI must be 6..15 digits (TS33128Payloads IMSI ::= NumericString (SIZE(6..15)))");
        }
        out.present = SUPI_PR_iMSI;
        if (set_numeric_string(out.choice.iMSI, imsi->digits) != 0) {
            return tl::unexpected("IMSI allocation failed");
        }
    } else {
        const auto& nai = std::get<Nai>(supi);
        out.present = SUPI_PR_nAI;
        if (OCTET_STRING_fromBuf(
                &out.choice.nAI, nai.value.data(), static_cast<int>(nai.value.size())) != 0) {
            return tl::unexpected("NAI allocation failed");
        }
    }
    return {};
}

tl::expected<Supi, std::string> extract_supi(const SUPI_t& src) {
    switch (src.present) {
        case SUPI_PR_iMSI:
            return Supi{Imsi{octet_string_to_std(src.choice.iMSI)}};
        case SUPI_PR_nAI:
            return Supi{Nai{octet_string_to_std(src.choice.nAI)}};
        default:
            return tl::unexpected("SUPI CHOICE has no alternative");
    }
}

// FiveGGUTI ::= SEQUENCE { mCC, mNC, aMFRegionID, aMFSetID, aMFPointer, fiveGTMSI }. Shared.
tl::expected<void, std::string> fill_guti(FiveGGUTI_t& out, const FiveGGuti& guti) {
    if (guti.mcc.size() != 3) {
        return tl::unexpected("MCC must be 3 digits (MCC ::= NumericString (SIZE(3)))");
    }
    if (guti.mnc.size() < 2 || guti.mnc.size() > 3) {
        return tl::unexpected("MNC must be 2..3 digits (MNC ::= NumericString (SIZE(2..3)))");
    }
    if (guti.amf_set_id > 1023) {
        return tl::unexpected("AMFSetID exceeds INTEGER (0..1023)");
    }
    if (guti.amf_pointer > 63) {
        return tl::unexpected("AMFPointer exceeds INTEGER (0..63)");
    }
    if (set_numeric_string(out.mCC, guti.mcc) != 0 || set_numeric_string(out.mNC, guti.mnc) != 0) {
        return tl::unexpected("GUTI allocation failed");
    }
    out.aMFRegionID = guti.amf_region_id;
    out.aMFSetID = guti.amf_set_id;
    out.aMFPointer = guti.amf_pointer;
    out.fiveGTMSI = guti.five_g_tmsi;
    return {};
}

FiveGGuti extract_guti(const FiveGGUTI_t& src) {
    FiveGGuti out;
    out.mcc = octet_string_to_std(src.mCC);
    out.mnc = octet_string_to_std(src.mNC);
    out.amf_region_id = static_cast<std::uint8_t>(src.aMFRegionID);
    out.amf_set_id = static_cast<std::uint16_t>(src.aMFSetID);
    out.amf_pointer = static_cast<std::uint8_t>(src.aMFPointer);
    out.five_g_tmsi = static_cast<std::uint32_t>(src.fiveGTMSI);
    return out;
}

// asn1c represents an OPTIONAL SEQUENCE member as a pointer the parent's ASN_STRUCT_FREE walks;
// calloc-zero it so unset OPTIONAL members inside stay absent.
template <typename T> T* alloc_optional() {
    return static_cast<T*>(calloc(1, sizeof(T)));
}

// A BIT STRING (SIZE(n)) holds a fixed-width integer left-aligned in ceil(n/8) octets, with the
// low `bits_unused` bits of the last octet zero. NRCellID is 36 bits, EUTRACellID 28.
int set_bit_string(BIT_STRING_t& bs, std::uint64_t value, unsigned bits) {
    const unsigned bytes = (bits + 7) / 8;
    const unsigned unused = bytes * 8 - bits;
    auto* buf = static_cast<std::uint8_t*>(calloc(bytes, 1));
    if (buf == nullptr) {
        return -1;
    }
    const std::uint64_t packed = value << unused;
    for (unsigned i = 0; i < bytes; ++i) {
        buf[bytes - 1 - i] = static_cast<std::uint8_t>(packed >> (8 * i));
    }
    bs.buf = buf;
    bs.size = static_cast<int>(bytes);
    bs.bits_unused = static_cast<int>(unused);
    return 0;
}

std::uint64_t get_bit_string(const BIT_STRING_t& bs) {
    std::uint64_t v = 0;
    for (int i = 0; i < bs.size; ++i) {
        v = (v << 8) | bs.buf[i];
    }
    return v >> static_cast<unsigned>(bs.bits_unused);
}

tl::expected<void, std::string> fill_plmnid(PLMNID_t& out, const Plmnid& plmn) {
    if (plmn.mcc.size() != 3) {
        return tl::unexpected("MCC must be 3 digits (MCC ::= NumericString (SIZE(3)))");
    }
    if (plmn.mnc.size() < 2 || plmn.mnc.size() > 3) {
        return tl::unexpected("MNC must be 2..3 digits (MNC ::= NumericString (SIZE(2..3)))");
    }
    if (set_numeric_string(out.mCC, plmn.mcc) != 0 || set_numeric_string(out.mNC, plmn.mnc) != 0) {
        return tl::unexpected("PLMNID allocation failed");
    }
    return {};
}

Plmnid extract_plmnid(const PLMNID_t& src) {
    return Plmnid{octet_string_to_std(src.mCC), octet_string_to_std(src.mNC)};
}

tl::expected<void, std::string> fill_tai(TAI_t& out, const Tai& tai) {
    if (auto r = fill_plmnid(out.pLMNID, tai.plmn); !r) {
        return r;
    }
    if (tai.tac.size() < 2 || tai.tac.size() > 3) {
        return tl::unexpected("TAC must be 2..3 octets (TAC ::= OCTET STRING (SIZE(2..3)))");
    }
    if (OCTET_STRING_fromBuf(&out.tAC,
                             reinterpret_cast<const char*>(tai.tac.data()),
                             static_cast<int>(tai.tac.size())) != 0) {
        return tl::unexpected("TAC allocation failed");
    }
    return {};
}

Tai extract_tai(const TAI_t& src) {
    Tai out;
    out.plmn = extract_plmnid(src.pLMNID);
    out.tac.assign(src.tAC.buf, src.tAC.buf + src.tAC.size);
    return out;
}

tl::expected<void, std::string> fill_ncgi(NCGI_t& out, const Ncgi& ncgi) {
    if (auto r = fill_plmnid(out.pLMNID, ncgi.plmn); !r) {
        return r;
    }
    if (ncgi.nr_cell_id >= (std::uint64_t{1} << 36)) {
        return tl::unexpected("NRCellID exceeds BIT STRING (SIZE(36))");
    }
    if (set_bit_string(out.nRCellID, ncgi.nr_cell_id, 36) != 0) {
        return tl::unexpected("NRCellID allocation failed");
    }
    return {};
}

Ncgi extract_ncgi(const NCGI_t& src) {
    Ncgi out;
    out.plmn = extract_plmnid(src.pLMNID);
    out.nr_cell_id = get_bit_string(src.nRCellID);
    return out;
}

tl::expected<void, std::string> fill_ecgi(ECGI_t& out, const Ecgi& ecgi) {
    if (auto r = fill_plmnid(out.pLMNID, ecgi.plmn); !r) {
        return r;
    }
    if (ecgi.eutra_cell_id >= (std::uint32_t{1} << 28)) {
        return tl::unexpected("EUTRACellID exceeds BIT STRING (SIZE(28))");
    }
    if (set_bit_string(out.eUTRACellID, ecgi.eutra_cell_id, 28) != 0) {
        return tl::unexpected("EUTRACellID allocation failed");
    }
    return {};
}

Ecgi extract_ecgi(const ECGI_t& src) {
    Ecgi out;
    out.plmn = extract_plmnid(src.pLMNID);
    out.eutra_cell_id = static_cast<std::uint32_t>(get_bit_string(src.eUTRACellID));
    return out;
}

// Location -> LocationInfo -> UserLocation, allocating each OPTIONAL level only when there is
// something below it to carry.
tl::expected<void, std::string> fill_location(Location_t& out, const Location& loc) {
    if (!loc.user_location) {
        return {}; // Location has no mandatory members; an empty one is valid
    }
    const UserLocation& ul = *loc.user_location;
    auto* info = alloc_optional<LocationInfo_t>();
    if (info == nullptr) {
        return tl::unexpected("LocationInfo allocation failed");
    }
    out.locationInfo = info;
    auto* user = alloc_optional<UserLocation_t>();
    if (user == nullptr) {
        return tl::unexpected("UserLocation allocation failed");
    }
    info->userLocation = user;
    if (ul.nr) {
        auto* nr = alloc_optional<NRLocation_t>();
        if (nr == nullptr) {
            return tl::unexpected("NRLocation allocation failed");
        }
        user->nRLocation = nr;
        if (auto r = fill_tai(nr->tAI, ul.nr->tai); !r) {
            return r;
        }
        if (auto r = fill_ncgi(nr->nCGI, ul.nr->ncgi); !r) {
            return r;
        }
    }
    if (ul.eutra) {
        auto* eutra = alloc_optional<EUTRALocation_t>();
        if (eutra == nullptr) {
            return tl::unexpected("EUTRALocation allocation failed");
        }
        user->eUTRALocation = eutra;
        if (auto r = fill_tai(eutra->tAI, ul.eutra->tai); !r) {
            return r;
        }
        if (auto r = fill_ecgi(eutra->eCGI, ul.eutra->ecgi); !r) {
            return r;
        }
    }
    return {};
}

Location extract_location(const Location_t& src) {
    Location out;
    if (src.locationInfo == nullptr || src.locationInfo->userLocation == nullptr) {
        return out;
    }
    const UserLocation_t& ul = *src.locationInfo->userLocation;
    UserLocation user;
    if (ul.nRLocation != nullptr) {
        NrLocation nr;
        nr.tai = extract_tai(ul.nRLocation->tAI);
        nr.ncgi = extract_ncgi(ul.nRLocation->nCGI);
        user.nr = nr;
    }
    if (ul.eUTRALocation != nullptr) {
        EutraLocation eutra;
        eutra.tai = extract_tai(ul.eUTRALocation->tAI);
        eutra.ecgi = extract_ecgi(ul.eUTRALocation->eCGI);
        user.eutra = eutra;
    }
    out.user_location = user;
    return out;
}

tl::expected<void, std::string> fill(AMFRegistration_t& out, const AmfRegistration& src) {
    out.registrationType = static_cast<long>(src.registration_type);
    out.registrationResult = static_cast<long>(src.registration_result);
    if (auto r = fill_supi(out.sUPI, src.supi); !r) {
        return r;
    }
    return fill_guti(out.gUTI, src.guti);
}

tl::expected<AmfRegistration, std::string> extract(const AMFRegistration_t& src) {
    AmfRegistration out;
    out.registration_type = static_cast<AmfRegistrationType>(src.registrationType);
    out.registration_result = static_cast<AmfRegistrationResult>(src.registrationResult);
    auto supi = extract_supi(src.sUPI);
    if (!supi) {
        return tl::unexpected(supi.error());
    }
    out.supi = *supi;
    out.guti = extract_guti(src.gUTI);
    return out;
}

void fill(AMFDeregistration_t& out, const AmfDeregistration& src) {
    // Both members are non-OPTIONAL in the ASN.1 and M in table 6.2.2.2.3-1; no allocation.
    out.deregistrationDirection = static_cast<long>(src.deregistration_direction);
    out.accessType = static_cast<long>(src.access_type);
}

AmfDeregistration extract(const AMFDeregistration_t& src) {
    AmfDeregistration out;
    out.deregistration_direction = static_cast<AmfDirection>(src.deregistrationDirection);
    out.access_type = static_cast<AccessType>(src.accessType);
    return out;
}

tl::expected<void, std::string> fill(AMFStartOfInterceptionWithRegisteredUE_t& out,
                                     const AmfStartOfInterceptionWithRegisteredUE& src) {
    // registrationResult, sUPI, gUTI are the non-OPTIONAL / M members; registrationType and the
    // rest are C/O (asn1c leaves the OPTIONAL pointers null, i.e. absent).
    out.registrationResult = static_cast<long>(src.registration_result);
    if (auto r = fill_supi(out.sUPI, src.supi); !r) {
        return r;
    }
    return fill_guti(out.gUTI, src.guti);
}

tl::expected<AmfStartOfInterceptionWithRegisteredUE, std::string>
extract(const AMFStartOfInterceptionWithRegisteredUE_t& src) {
    AmfStartOfInterceptionWithRegisteredUE out;
    out.registration_result = static_cast<AmfRegistrationResult>(src.registrationResult);
    auto supi = extract_supi(src.sUPI);
    if (!supi) {
        return tl::unexpected(supi.error());
    }
    out.supi = *supi;
    out.guti = extract_guti(src.gUTI);
    return out;
}

tl::expected<void, std::string> fill(AMFLocationUpdate_t& out, const AmfLocationUpdate& src) {
    // sUPI and location are the M members; location is embedded (non-OPTIONAL) so it is filled in
    // place, everything below it is the OPTIONAL user-location chain.
    if (auto r = fill_supi(out.sUPI, src.supi); !r) {
        return r;
    }
    return fill_location(out.location, src.location);
}

tl::expected<AmfLocationUpdate, std::string> extract(const AMFLocationUpdate_t& src) {
    AmfLocationUpdate out;
    auto supi = extract_supi(src.sUPI);
    if (!supi) {
        return tl::unexpected(supi.error());
    }
    out.supi = *supi;
    out.location = extract_location(src.location);
    return out;
}

} // namespace

tl::expected<std::vector<std::uint8_t>, std::string> encode_xiri_payload(const Event& event) {
    PayloadPtr payload = new_payload();
    if (!payload) {
        return tl::unexpected("allocation failed");
    }
    if (RELATIVE_OID_set_arcs(
            &payload->xIRIPayloadOID, kXiriPayloadOidArcs, std::size(kXiriPayloadOidArcs)) != 0) {
        return tl::unexpected("xIRIPayloadOID allocation failed");
    }

    tl::expected<void, std::string> filled = std::visit(
        [&](const auto& variant_event) -> tl::expected<void, std::string> {
            using T = std::decay_t<decltype(variant_event)>;
            if constexpr (std::is_same_v<T, AmfRegistration>) {
                payload->event.present = XIRIEvent_PR_registration;
                return fill(payload->event.choice.registration, variant_event);
            } else if constexpr (std::is_same_v<T, AmfDeregistration>) {
                payload->event.present = XIRIEvent_PR_deregistration;
                fill(payload->event.choice.deregistration, variant_event);
                return {};
            } else if constexpr (std::is_same_v<T, AmfStartOfInterceptionWithRegisteredUE>) {
                payload->event.present = XIRIEvent_PR_startOfInterceptionWithRegisteredUE;
                return fill(payload->event.choice.startOfInterceptionWithRegisteredUE,
                            variant_event);
            } else if constexpr (std::is_same_v<T, AmfLocationUpdate>) {
                payload->event.present = XIRIEvent_PR_locationUpdate;
                return fill(payload->event.choice.locationUpdate, variant_event);
            }
        },
        event);
    if (!filled) {
        return tl::unexpected(filled.error());
    }

    // der_encode streams through a callback; appending to a vector avoids a sizing pass.
    std::vector<std::uint8_t> out;
    asn_enc_rval_t rv = der_encode(
        &asn_DEF_XIRIPayload,
        payload.get(),
        [](const void* data, std::size_t size, void* key) -> int {
            auto* sink = static_cast<std::vector<std::uint8_t>*>(key);
            const auto* octets = static_cast<const std::uint8_t*>(data);
            sink->insert(sink->end(), octets, octets + size);
            return 0;
        },
        &out);
    if (rv.encoded < 0 || static_cast<std::size_t>(rv.encoded) != out.size()) {
        return tl::unexpected(std::string("DER encode failed at ") +
                              (rv.failed_type ? rv.failed_type->name : "?"));
    }
    return out;
}

tl::expected<DecodedXiri, std::string> decode_xiri_payload(std::span<const std::uint8_t> bytes) {
    XIRIPayload_t* raw = nullptr;
    asn_dec_rval_t rv = ber_decode(
        nullptr, &asn_DEF_XIRIPayload, reinterpret_cast<void**>(&raw), bytes.data(), bytes.size());
    PayloadPtr payload(raw);
    if (rv.code != RC_OK) {
        return tl::unexpected(rv.code == RC_WMORE ? "BER decode: truncated XIRIPayload"
                                                  : "BER decode: malformed XIRIPayload");
    }
    if (rv.consumed != bytes.size()) {
        return tl::unexpected("BER decode: trailing bytes after XIRIPayload");
    }

    DecodedXiri out;
    asn_oid_arc_t arcs[16];
    const ssize_t arc_count =
        RELATIVE_OID_get_arcs(&payload->xIRIPayloadOID, arcs, std::size(arcs));
    if (arc_count < 0 || static_cast<std::size_t>(arc_count) > std::size(arcs)) {
        return tl::unexpected("xIRIPayloadOID unreadable");
    }
    out.payload_oid.assign(arcs, arcs + arc_count);

    switch (payload->event.present) {
        case XIRIEvent_PR_registration: {
            auto registration = extract(payload->event.choice.registration);
            if (!registration) {
                return tl::unexpected(registration.error());
            }
            out.event = *registration;
            return out;
        }
        case XIRIEvent_PR_deregistration:
            out.event = extract(payload->event.choice.deregistration);
            return out;
        case XIRIEvent_PR_startOfInterceptionWithRegisteredUE: {
            auto soi = extract(payload->event.choice.startOfInterceptionWithRegisteredUE);
            if (!soi) {
                return tl::unexpected(soi.error());
            }
            out.event = *soi;
            return out;
        }
        case XIRIEvent_PR_locationUpdate: {
            auto location_update = extract(payload->event.choice.locationUpdate);
            if (!location_update) {
                return tl::unexpected(location_update.error());
            }
            out.event = *location_update;
            return out;
        }
        default:
            return tl::unexpected("XIRIEvent alternative " +
                                  std::to_string(static_cast<int>(payload->event.present)) +
                                  " is not modelled by li_core::xiri yet");
    }
}

} // namespace li_core::xiri
