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
#include <AMFIdentifierAssociation.h>
#include <AMFIdentifierDeassociation.h>
#include <AMFLocationUpdate.h>
#include <AMFRegistration.h>
#include <AMFStartOfInterceptionWithRegisteredUE.h>
#include <BIT_STRING.h>
#include <ECGI.h>
#include <EUTRALocation.h>
#include <FTEID.h>
#include <GPSI.h>
#include <Location.h>
#include <LocationInfo.h>
#include <NCGI.h>
#include <NRLocation.h>
#include <NumericString.h>
#include <OBJECT_IDENTIFIER.h>
#include <OCTET_STRING.h>
#include <PEI.h>
#include <PLMNID.h>
#include <RELATIVE-OID.h>
#include <SMFPDUSessionEstablishment.h>
#include <SMFPDUSessionModification.h>
#include <SMFPDUSessionRelease.h>
#include <SMFStartOfInterceptionWithEstablishedPDUSession.h>
#include <SMFUnsuccessfulProcedure.h>
#include <SNSSAI.h>
#include <TAI.h>
#include <UEEndpointAddress.h>
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

tl::expected<void, std::string> fill(AMFDeregistration_t& out, const AmfDeregistration& src) {
    // deregistrationDirection/accessType are non-OPTIONAL (M); everything below is an asn1c
    // OPTIONAL pointer, allocated only when this project's POI has the value (ADR-0393).
    out.deregistrationDirection = static_cast<long>(src.deregistration_direction);
    out.accessType = static_cast<long>(src.access_type);
    if (src.supi) {
        auto* supi = alloc_optional<SUPI_t>();
        if (supi == nullptr) {
            return tl::unexpected("SUPI allocation failed");
        }
        out.sUPI = supi;
        if (auto r = fill_supi(*supi, *src.supi); !r) {
            return r;
        }
    }
    if (src.guti) {
        auto* guti = alloc_optional<FiveGGUTI_t>();
        if (guti == nullptr) {
            return tl::unexpected("FiveGGUTI allocation failed");
        }
        out.gUTI = guti;
        if (auto r = fill_guti(*guti, *src.guti); !r) {
            return r;
        }
    }
    if (src.location) {
        auto* loc = alloc_optional<Location_t>();
        if (loc == nullptr) {
            return tl::unexpected("Location allocation failed");
        }
        out.location = loc;
        if (auto r = fill_location(*loc, *src.location); !r) {
            return r;
        }
    }
    if (src.switch_off_indicator) {
        auto* sw = alloc_optional<SwitchOffIndicator_t>();
        if (sw == nullptr) {
            return tl::unexpected("SwitchOffIndicator allocation failed");
        }
        *sw = static_cast<long>(*src.switch_off_indicator);
        out.switchOffIndicator = sw;
    }
    return {};
}

tl::expected<AmfDeregistration, std::string> extract(const AMFDeregistration_t& src) {
    AmfDeregistration out;
    out.deregistration_direction = static_cast<AmfDirection>(src.deregistrationDirection);
    out.access_type = static_cast<AccessType>(src.accessType);
    if (src.sUPI != nullptr) {
        auto supi = extract_supi(*src.sUPI);
        if (!supi) {
            return tl::unexpected(supi.error());
        }
        out.supi = *supi;
    }
    if (src.gUTI != nullptr) {
        out.guti = extract_guti(*src.gUTI);
    }
    if (src.location != nullptr) {
        out.location = extract_location(*src.location);
    }
    if (src.switchOffIndicator != nullptr) {
        out.switch_off_indicator =
            static_cast<AmfDeregistration::SwitchOff>(*src.switchOffIndicator);
    }
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
    if (auto r = fill_guti(out.gUTI, src.guti); !r) {
        return r;
    }
    if (src.location) {
        auto* loc = alloc_optional<Location_t>();
        if (loc == nullptr) {
            return tl::unexpected("Location allocation failed");
        }
        out.location = loc;
        if (auto r = fill_location(*loc, *src.location); !r) {
            return r;
        }
    }
    if (src.time_of_registration) {
        const std::string& t = *src.time_of_registration;
        // GeneralizedTime in UTC: 14 digits then 'Z' (optionally with fractional seconds).
        if (t.size() < 15 || t.back() != 'Z') {
            return tl::unexpected("timeOfRegistration must be a UTC GeneralizedTime ending in 'Z'");
        }
        auto* ts = alloc_optional<Timestamp_t>();
        if (ts == nullptr) {
            return tl::unexpected("Timestamp allocation failed");
        }
        out.timeOfRegistration = ts;
        if (OCTET_STRING_fromBuf(ts, t.data(), static_cast<int>(t.size())) != 0) {
            return tl::unexpected("timeOfRegistration allocation failed");
        }
    }
    return {};
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
    if (src.location != nullptr) {
        out.location = extract_location(*src.location);
    }
    if (src.timeOfRegistration != nullptr) {
        out.time_of_registration =
            std::string(reinterpret_cast<const char*>(src.timeOfRegistration->buf),
                        src.timeOfRegistration->size);
    }
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

tl::expected<void, std::string> fill(AMFIdentifierAssociation_t& out,
                                     const AmfIdentifierAssociation& src) {
    // sUPI, gUTI, location are all M (non-OPTIONAL); location is embedded, filled in place.
    if (auto r = fill_supi(out.sUPI, src.supi); !r) {
        return r;
    }
    if (auto r = fill_guti(out.gUTI, src.guti); !r) {
        return r;
    }
    return fill_location(out.location, src.location);
}

tl::expected<AmfIdentifierAssociation, std::string> extract(const AMFIdentifierAssociation_t& src) {
    AmfIdentifierAssociation out;
    auto supi = extract_supi(src.sUPI);
    if (!supi) {
        return tl::unexpected(supi.error());
    }
    out.supi = *supi;
    out.guti = extract_guti(src.gUTI);
    out.location = extract_location(src.location);
    return out;
}

tl::expected<void, std::string> fill(AMFIdentifierDeassociation_t& out,
                                     const AmfIdentifierDeassociation& src) {
    // sUPI, gUTI are M; location is OPTIONAL (an asn1c pointer allocated only when present).
    if (auto r = fill_supi(out.sUPI, src.supi); !r) {
        return r;
    }
    if (auto r = fill_guti(out.gUTI, src.guti); !r) {
        return r;
    }
    if (src.location) {
        auto* loc = alloc_optional<Location_t>();
        if (loc == nullptr) {
            return tl::unexpected("Location allocation failed");
        }
        out.location = loc;
        return fill_location(*loc, *src.location);
    }
    return {};
}

tl::expected<AmfIdentifierDeassociation, std::string>
extract(const AMFIdentifierDeassociation_t& src) {
    AmfIdentifierDeassociation out;
    auto supi = extract_supi(src.sUPI);
    if (!supi) {
        return tl::unexpected(supi.error());
    }
    out.supi = *supi;
    out.guti = extract_guti(src.gUTI);
    if (src.location != nullptr) {
        out.location = extract_location(*src.location);
    }
    return out;
}

// ---- SMF xIRI records (ADR-0463)
// ------------------------------------------------------------------

template <std::size_t N>
int set_octets(OCTET_STRING_t& dst, const std::array<std::uint8_t, N>& bytes) {
    return OCTET_STRING_fromBuf(
        &dst, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(N));
}

template <std::size_t N> std::array<std::uint8_t, N> to_array(const OCTET_STRING_t& src) {
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N && i < static_cast<std::size_t>(src.size); ++i) {
        out[i] = src.buf[i];
    }
    return out;
}

tl::expected<void, std::string> fill_fteid(FTEID_t& out, const Fteid& f) {
    out.tEID = f.teid;
    if (f.ipv4) {
        auto* a = alloc_optional<IPv4Address_t>();
        if (a == nullptr) {
            return tl::unexpected("F-TEID IPv4 allocation failed");
        }
        out.iPv4Address = a;
        if (set_octets(*a, *f.ipv4) != 0) {
            return tl::unexpected("F-TEID IPv4 allocation failed");
        }
    }
    if (f.ipv6) {
        auto* a = alloc_optional<IPv6Address_t>();
        if (a == nullptr) {
            return tl::unexpected("F-TEID IPv6 allocation failed");
        }
        out.iPv6Address = a;
        if (set_octets(*a, *f.ipv6) != 0) {
            return tl::unexpected("F-TEID IPv6 allocation failed");
        }
    }
    return {};
}

Fteid extract_fteid(const FTEID_t& src) {
    Fteid f;
    f.teid = static_cast<std::uint32_t>(src.tEID);
    if (src.iPv4Address != nullptr) {
        f.ipv4 = to_array<4>(*src.iPv4Address);
    }
    if (src.iPv6Address != nullptr) {
        f.ipv6 = to_array<16>(*src.iPv6Address);
    }
    return f;
}

tl::expected<void, std::string> fill_snssai(SNSSAI_t& out, const Snssai& v) {
    out.sliceServiceType = v.sst;
    if (v.sd) {
        auto* sd = alloc_optional<OCTET_STRING_t>();
        if (sd == nullptr) {
            return tl::unexpected("S-NSSAI SD allocation failed");
        }
        out.sliceDifferentiator = sd;
        if (set_octets(*sd, *v.sd) != 0) {
            return tl::unexpected("S-NSSAI SD allocation failed");
        }
    }
    return {};
}

Snssai extract_snssai(const SNSSAI_t& src) {
    Snssai v;
    v.sst = static_cast<std::uint8_t>(src.sliceServiceType);
    if (src.sliceDifferentiator != nullptr) {
        v.sd = to_array<3>(*src.sliceDifferentiator);
    }
    return v;
}

tl::expected<void, std::string> fill_ue_endpoint(UEEndpointAddress_t& out, const UeEndpoint& e) {
    int rc = 0;
    if (const auto* v4 = std::get_if<std::array<std::uint8_t, 4>>(&e)) {
        out.present = UEEndpointAddress_PR_iPv4Address;
        rc = set_octets(out.choice.iPv4Address, *v4);
    } else if (const auto* v6 = std::get_if<std::array<std::uint8_t, 16>>(&e)) {
        out.present = UEEndpointAddress_PR_iPv6Address;
        rc = set_octets(out.choice.iPv6Address, *v6);
    } else {
        out.present = UEEndpointAddress_PR_ethernetAddress;
        rc = set_octets(out.choice.ethernetAddress, std::get<std::array<std::uint8_t, 6>>(e));
    }
    return rc == 0 ? tl::expected<void, std::string>{}
                   : tl::unexpected("UE endpoint allocation failed");
}

tl::expected<UeEndpoint, std::string> extract_ue_endpoint(const UEEndpointAddress_t& src) {
    switch (src.present) {
        case UEEndpointAddress_PR_iPv4Address:
            return UeEndpoint{to_array<4>(src.choice.iPv4Address)};
        case UEEndpointAddress_PR_iPv6Address:
            return UeEndpoint{to_array<16>(src.choice.iPv6Address)};
        case UEEndpointAddress_PR_ethernetAddress:
            return UeEndpoint{to_array<6>(src.choice.ethernetAddress)};
        default:
            return tl::unexpected("UEEndpointAddress CHOICE has no alternative");
    }
}

// A SEQUENCE OF UEEndpointAddress (asn1c: a struct holding A_SEQUENCE_OF list).
template <typename ListOwner>
tl::expected<void, std::string> fill_endpoint_list(ListOwner& owner,
                                                   const std::vector<UeEndpoint>& endpoints) {
    for (const auto& e : endpoints) {
        auto* a = alloc_optional<UEEndpointAddress_t>();
        if (a == nullptr) {
            return tl::unexpected("UE endpoint allocation failed");
        }
        if (ASN_SEQUENCE_ADD(&owner.list, a) != 0) {
            ASN_STRUCT_FREE(asn_DEF_UEEndpointAddress, a);
            return tl::unexpected("UE endpoint list allocation failed");
        }
        if (auto r = fill_ue_endpoint(*a, e); !r) {
            return r;
        }
    }
    return {};
}

template <typename ListOwner>
tl::expected<std::vector<UeEndpoint>, std::string> extract_endpoint_list(const ListOwner& owner) {
    std::vector<UeEndpoint> out;
    for (int i = 0; i < owner.list.count; ++i) {
        auto e = extract_ue_endpoint(*owner.list.array[i]);
        if (!e) {
            return tl::unexpected(e.error());
        }
        out.push_back(*e);
    }
    return out;
}

tl::expected<void, std::string> fill_pei(PEI_t& out, const Pei& pei) {
    if (const auto* imei = std::get_if<Imei>(&pei)) {
        if (imei->digits.size() != 14) {
            return tl::unexpected("IMEI must be 14 digits (IMEI ::= NumericString (SIZE(14)))");
        }
        out.present = PEI_PR_iMEI;
        return set_numeric_string(out.choice.iMEI, imei->digits) == 0
                   ? tl::expected<void, std::string>{}
                   : tl::unexpected("IMEI allocation failed");
    }
    const auto& sv = std::get<Imeisv>(pei);
    if (sv.digits.size() != 16) {
        return tl::unexpected("IMEISV must be 16 digits (IMEISV ::= NumericString (SIZE(16)))");
    }
    out.present = PEI_PR_iMEISV;
    return set_numeric_string(out.choice.iMEISV, sv.digits) == 0
               ? tl::expected<void, std::string>{}
               : tl::unexpected("IMEISV allocation failed");
}

tl::expected<Pei, std::string> extract_pei(const PEI_t& src) {
    switch (src.present) {
        case PEI_PR_iMEI:
            return Pei{Imei{octet_string_to_std(src.choice.iMEI)}};
        case PEI_PR_iMEISV:
            return Pei{Imeisv{octet_string_to_std(src.choice.iMEISV)}};
        default:
            return tl::unexpected("PEI alternative is not modelled (only IMEI / IMEISV)");
    }
}

tl::expected<void, std::string> fill_gpsi(GPSI_t& out, const Gpsi& g) {
    if (const auto* m = std::get_if<Msisdn>(&g)) {
        if (m->digits.empty() || m->digits.size() > 15) {
            return tl::unexpected(
                "MSISDN must be 1..15 digits (MSISDN ::= NumericString (SIZE(1..15)))");
        }
        out.present = GPSI_PR_mSISDN;
        return set_numeric_string(out.choice.mSISDN, m->digits) == 0
                   ? tl::expected<void, std::string>{}
                   : tl::unexpected("MSISDN allocation failed");
    }
    const auto& nai = std::get<Nai>(g);
    out.present = GPSI_PR_nAI;
    return OCTET_STRING_fromBuf(
               &out.choice.nAI, nai.value.data(), static_cast<int>(nai.value.size())) == 0
               ? tl::expected<void, std::string>{}
               : tl::unexpected("GPSI NAI allocation failed");
}

tl::expected<Gpsi, std::string> extract_gpsi(const GPSI_t& src) {
    switch (src.present) {
        case GPSI_PR_mSISDN:
            return Gpsi{Msisdn{octet_string_to_std(src.choice.mSISDN)}};
        case GPSI_PR_nAI:
            return Gpsi{Nai{octet_string_to_std(src.choice.nAI)}};
        default:
            return tl::unexpected("GPSI CHOICE has no alternative");
    }
}

// sUPI / pEI / gPSI of the records that carry sUPI as an OPTIONAL pointer.
template <typename Rec>
tl::expected<void, std::string> fill_ids(Rec& out, const SmIdentities& ids) {
    if (ids.supi) {
        out.sUPI = alloc_optional<SUPI_t>();
        if (out.sUPI == nullptr) {
            return tl::unexpected("SUPI allocation failed");
        }
        if (auto r = fill_supi(*out.sUPI, *ids.supi); !r) {
            return r;
        }
    }
    if (ids.pei) {
        out.pEI = alloc_optional<PEI_t>();
        if (out.pEI == nullptr) {
            return tl::unexpected("PEI allocation failed");
        }
        if (auto r = fill_pei(*out.pEI, *ids.pei); !r) {
            return r;
        }
    }
    if (ids.gpsi) {
        out.gPSI = alloc_optional<GPSI_t>();
        if (out.gPSI == nullptr) {
            return tl::unexpected("GPSI allocation failed");
        }
        if (auto r = fill_gpsi(*out.gPSI, *ids.gpsi); !r) {
            return r;
        }
    }
    return {};
}

template <typename Rec> tl::expected<SmIdentities, std::string> extract_ids(const Rec& src) {
    SmIdentities ids;
    if (src.sUPI != nullptr) {
        auto v = extract_supi(*src.sUPI);
        if (!v) {
            return tl::unexpected(v.error());
        }
        ids.supi = *v;
    }
    if (src.pEI != nullptr) {
        auto v = extract_pei(*src.pEI);
        if (!v) {
            return tl::unexpected(v.error());
        }
        ids.pei = *v;
    }
    if (src.gPSI != nullptr) {
        auto v = extract_gpsi(*src.gPSI);
        if (!v) {
            return tl::unexpected(v.error());
        }
        ids.gpsi = *v;
    }
    return ids;
}

tl::expected<void, std::string> fill_dnn(DNN_t& out, const std::string& dnn) {
    if (dnn.empty()) {
        return tl::unexpected("DNN must not be empty");
    }
    return OCTET_STRING_fromBuf(&out, dnn.data(), static_cast<int>(dnn.size())) == 0
               ? tl::expected<void, std::string>{}
               : tl::unexpected("DNN allocation failed");
}

template <typename Rec>
tl::expected<void, std::string> fill_optional_location(Rec& out,
                                                       const std::optional<Location>& loc) {
    if (!loc) {
        return {};
    }
    out.location = alloc_optional<Location_t>();
    if (out.location == nullptr) {
        return tl::unexpected("Location allocation failed");
    }
    return fill_location(*out.location, *loc);
}

template <typename Rec>
tl::expected<void, std::string> fill_optional_access(Rec& out, const std::optional<AccessType>& a) {
    if (!a) {
        return {};
    }
    out.accessType = alloc_optional<AccessType_t>();
    if (out.accessType == nullptr) {
        return tl::unexpected("AccessType allocation failed");
    }
    *out.accessType = static_cast<long>(*a);
    return {};
}

template <typename Rec>
tl::expected<void, std::string> fill_optional_snssai(Rec& out, const std::optional<Snssai>& v) {
    if (!v) {
        return {};
    }
    out.sNSSAI = alloc_optional<SNSSAI_t>();
    if (out.sNSSAI == nullptr) {
        return tl::unexpected("S-NSSAI allocation failed");
    }
    return fill_snssai(*out.sNSSAI, *v);
}

#define LI_TRY(expr)                                                                               \
    do {                                                                                           \
        if (auto r_ = (expr); !r_) {                                                               \
            return r_;                                                                             \
        }                                                                                          \
    } while (false)

tl::expected<void, std::string> fill(SMFPDUSessionEstablishment_t& out,
                                     const SmfPduSessionEstablishment& e) {
    LI_TRY(fill_ids(out, e.ids));
    out.pDUSessionID = e.pdu_session_id;
    LI_TRY(fill_fteid(out.gTPTunnelID, e.gtp_tunnel));
    out.pDUSessionType = static_cast<long>(e.pdu_session_type);
    LI_TRY(fill_optional_snssai(out, e.snssai));
    if (!e.ue_endpoints.empty()) {
        out.uEEndpoint = alloc_optional<std::remove_pointer_t<decltype(out.uEEndpoint)>>();
        if (out.uEEndpoint == nullptr) {
            return tl::unexpected("UE endpoint list allocation failed");
        }
        LI_TRY(fill_endpoint_list(*out.uEEndpoint, e.ue_endpoints));
    }
    LI_TRY(fill_optional_location(out, e.location));
    LI_TRY(fill_dnn(out.dNN, e.dnn));
    out.requestType = static_cast<long>(e.request_type);
    LI_TRY(fill_optional_access(out, e.access_type));
    return {};
}

tl::expected<SmfPduSessionEstablishment, std::string>
extract(const SMFPDUSessionEstablishment_t& src) {
    SmfPduSessionEstablishment e;
    auto ids = extract_ids(src);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    e.ids = *ids;
    e.pdu_session_id = static_cast<std::uint8_t>(src.pDUSessionID);
    e.gtp_tunnel = extract_fteid(src.gTPTunnelID);
    e.pdu_session_type = static_cast<PduSessionType>(src.pDUSessionType);
    if (src.sNSSAI != nullptr) {
        e.snssai = extract_snssai(*src.sNSSAI);
    }
    if (src.uEEndpoint != nullptr) {
        auto list = extract_endpoint_list(*src.uEEndpoint);
        if (!list) {
            return tl::unexpected(list.error());
        }
        e.ue_endpoints = *list;
    }
    if (src.location != nullptr) {
        e.location = extract_location(*src.location);
    }
    e.dnn = octet_string_to_std(src.dNN);
    e.request_type = static_cast<SmRequestType>(src.requestType);
    if (src.accessType != nullptr) {
        e.access_type = static_cast<AccessType>(*src.accessType);
    }
    return e;
}

tl::expected<void, std::string> fill(SMFStartOfInterceptionWithEstablishedPDUSession_t& out,
                                     const SmfStartOfInterceptionWithEstablishedPduSession& e) {
    LI_TRY(fill_ids(out, e.ids));
    out.pDUSessionID = e.pdu_session_id;
    LI_TRY(fill_fteid(out.gTPTunnelID, e.gtp_tunnel));
    out.pDUSessionType = static_cast<long>(e.pdu_session_type);
    LI_TRY(fill_optional_snssai(out, e.snssai));
    LI_TRY(fill_endpoint_list(out.uEEndpoint, e.ue_endpoints)); // mandatory here, possibly empty
    LI_TRY(fill_optional_location(out, e.location));
    LI_TRY(fill_dnn(out.dNN, e.dnn));
    out.requestType = static_cast<long>(e.request_type);
    LI_TRY(fill_optional_access(out, e.access_type));
    return {};
}

tl::expected<SmfStartOfInterceptionWithEstablishedPduSession, std::string>
extract(const SMFStartOfInterceptionWithEstablishedPDUSession_t& src) {
    SmfStartOfInterceptionWithEstablishedPduSession e;
    auto ids = extract_ids(src);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    e.ids = *ids;
    e.pdu_session_id = static_cast<std::uint8_t>(src.pDUSessionID);
    e.gtp_tunnel = extract_fteid(src.gTPTunnelID);
    e.pdu_session_type = static_cast<PduSessionType>(src.pDUSessionType);
    if (src.sNSSAI != nullptr) {
        e.snssai = extract_snssai(*src.sNSSAI);
    }
    auto list = extract_endpoint_list(src.uEEndpoint);
    if (!list) {
        return tl::unexpected(list.error());
    }
    e.ue_endpoints = *list;
    if (src.location != nullptr) {
        e.location = extract_location(*src.location);
    }
    e.dnn = octet_string_to_std(src.dNN);
    e.request_type = static_cast<SmRequestType>(src.requestType);
    if (src.accessType != nullptr) {
        e.access_type = static_cast<AccessType>(*src.accessType);
    }
    return e;
}

tl::expected<void, std::string> fill(SMFPDUSessionModification_t& out,
                                     const SmfPduSessionModification& m) {
    LI_TRY(fill_ids(out, m.ids));
    LI_TRY(fill_optional_snssai(out, m.snssai));
    LI_TRY(fill_optional_location(out, m.location));
    out.requestType = static_cast<long>(m.request_type);
    LI_TRY(fill_optional_access(out, m.access_type));
    if (m.pdu_session_id) {
        out.pDUSessionID = alloc_optional<PDUSessionID_t>();
        if (out.pDUSessionID == nullptr) {
            return tl::unexpected("PDU session id allocation failed");
        }
        *out.pDUSessionID = *m.pdu_session_id;
    }
    if (m.ue_endpoint) {
        out.uEEndpoint = alloc_optional<UEEndpointAddress_t>();
        if (out.uEEndpoint == nullptr) {
            return tl::unexpected("UE endpoint allocation failed");
        }
        LI_TRY(fill_ue_endpoint(*out.uEEndpoint, *m.ue_endpoint));
    }
    return {};
}

tl::expected<SmfPduSessionModification, std::string>
extract(const SMFPDUSessionModification_t& src) {
    SmfPduSessionModification m;
    auto ids = extract_ids(src);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    m.ids = *ids;
    if (src.sNSSAI != nullptr) {
        m.snssai = extract_snssai(*src.sNSSAI);
    }
    if (src.location != nullptr) {
        m.location = extract_location(*src.location);
    }
    m.request_type = static_cast<SmRequestType>(src.requestType);
    if (src.accessType != nullptr) {
        m.access_type = static_cast<AccessType>(*src.accessType);
    }
    if (src.pDUSessionID != nullptr) {
        m.pdu_session_id = static_cast<std::uint8_t>(*src.pDUSessionID);
    }
    if (src.uEEndpoint != nullptr) {
        auto e = extract_ue_endpoint(*src.uEEndpoint);
        if (!e) {
            return tl::unexpected(e.error());
        }
        m.ue_endpoint = *e;
    }
    return m;
}

tl::expected<void, std::string> fill(SMFPDUSessionRelease_t& out, const SmfPduSessionRelease& r) {
    LI_TRY(fill_supi(out.sUPI, r.supi)); // sUPI is MANDATORY on a release
    if (r.pei) {
        out.pEI = alloc_optional<PEI_t>();
        if (out.pEI == nullptr) {
            return tl::unexpected("PEI allocation failed");
        }
        LI_TRY(fill_pei(*out.pEI, *r.pei));
    }
    if (r.gpsi) {
        out.gPSI = alloc_optional<GPSI_t>();
        if (out.gPSI == nullptr) {
            return tl::unexpected("GPSI allocation failed");
        }
        LI_TRY(fill_gpsi(*out.gPSI, *r.gpsi));
    }
    out.pDUSessionID = r.pdu_session_id;
    LI_TRY(fill_optional_location(out, r.location));
    return {};
}

tl::expected<SmfPduSessionRelease, std::string> extract(const SMFPDUSessionRelease_t& src) {
    SmfPduSessionRelease r;
    auto supi = extract_supi(src.sUPI);
    if (!supi) {
        return tl::unexpected(supi.error());
    }
    r.supi = *supi;
    if (src.pEI != nullptr) {
        auto v = extract_pei(*src.pEI);
        if (!v) {
            return tl::unexpected(v.error());
        }
        r.pei = *v;
    }
    if (src.gPSI != nullptr) {
        auto v = extract_gpsi(*src.gPSI);
        if (!v) {
            return tl::unexpected(v.error());
        }
        r.gpsi = *v;
    }
    r.pdu_session_id = static_cast<std::uint8_t>(src.pDUSessionID);
    if (src.location != nullptr) {
        r.location = extract_location(*src.location);
    }
    return r;
}

tl::expected<void, std::string> fill(SMFUnsuccessfulProcedure_t& out,
                                     const SmfUnsuccessfulProcedure& u) {
    out.failedProcedureType = static_cast<long>(u.failed_procedure);
    out.failureCause = u.failure_cause;
    out.initiator = static_cast<long>(u.initiator);
    LI_TRY(fill_ids(out, u.ids));
    if (u.pdu_session_id) {
        out.pDUSessionID = alloc_optional<PDUSessionID_t>();
        if (out.pDUSessionID == nullptr) {
            return tl::unexpected("PDU session id allocation failed");
        }
        *out.pDUSessionID = *u.pdu_session_id;
    }
    if (!u.ue_endpoints.empty()) {
        out.uEEndpoint = alloc_optional<std::remove_pointer_t<decltype(out.uEEndpoint)>>();
        if (out.uEEndpoint == nullptr) {
            return tl::unexpected("UE endpoint list allocation failed");
        }
        LI_TRY(fill_endpoint_list(*out.uEEndpoint, u.ue_endpoints));
    }
    if (u.dnn) {
        out.dNN = alloc_optional<DNN_t>();
        if (out.dNN == nullptr) {
            return tl::unexpected("DNN allocation failed");
        }
        LI_TRY(fill_dnn(*out.dNN, *u.dnn));
    }
    if (u.request_type) {
        out.requestType = alloc_optional<FiveGSMRequestType_t>();
        if (out.requestType == nullptr) {
            return tl::unexpected("request type allocation failed");
        }
        *out.requestType = static_cast<long>(*u.request_type);
    }
    LI_TRY(fill_optional_access(out, u.access_type));
    LI_TRY(fill_optional_location(out, u.location));
    return {};
}

tl::expected<SmfUnsuccessfulProcedure, std::string> extract(const SMFUnsuccessfulProcedure_t& src) {
    SmfUnsuccessfulProcedure u;
    u.failed_procedure = static_cast<SmFailedProcedure>(src.failedProcedureType);
    u.failure_cause = static_cast<std::uint8_t>(src.failureCause);
    u.initiator = static_cast<SmInitiator>(src.initiator);
    auto ids = extract_ids(src);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    u.ids = *ids;
    if (src.pDUSessionID != nullptr) {
        u.pdu_session_id = static_cast<std::uint8_t>(*src.pDUSessionID);
    }
    if (src.uEEndpoint != nullptr) {
        auto list = extract_endpoint_list(*src.uEEndpoint);
        if (!list) {
            return tl::unexpected(list.error());
        }
        u.ue_endpoints = *list;
    }
    if (src.dNN != nullptr) {
        u.dnn = octet_string_to_std(*src.dNN);
    }
    if (src.requestType != nullptr) {
        u.request_type = static_cast<SmRequestType>(*src.requestType);
    }
    if (src.accessType != nullptr) {
        u.access_type = static_cast<AccessType>(*src.accessType);
    }
    if (src.location != nullptr) {
        u.location = extract_location(*src.location);
    }
    return u;
}

#undef LI_TRY

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
                return fill(payload->event.choice.deregistration, variant_event);
            } else if constexpr (std::is_same_v<T, AmfStartOfInterceptionWithRegisteredUE>) {
                payload->event.present = XIRIEvent_PR_startOfInterceptionWithRegisteredUE;
                return fill(payload->event.choice.startOfInterceptionWithRegisteredUE,
                            variant_event);
            } else if constexpr (std::is_same_v<T, AmfLocationUpdate>) {
                payload->event.present = XIRIEvent_PR_locationUpdate;
                return fill(payload->event.choice.locationUpdate, variant_event);
            } else if constexpr (std::is_same_v<T, AmfIdentifierAssociation>) {
                payload->event.present = XIRIEvent_PR_aMFIdentifierAssociation;
                return fill(payload->event.choice.aMFIdentifierAssociation, variant_event);
            } else if constexpr (std::is_same_v<T, AmfIdentifierDeassociation>) {
                payload->event.present = XIRIEvent_PR_aMFIdentifierDeassociation;
                return fill(payload->event.choice.aMFIdentifierDeassociation, variant_event);
            } else if constexpr (std::is_same_v<T, SmfPduSessionEstablishment>) {
                payload->event.present = XIRIEvent_PR_pDUSessionEstablishment;
                return fill(payload->event.choice.pDUSessionEstablishment, variant_event);
            } else if constexpr (std::is_same_v<T, SmfPduSessionModification>) {
                payload->event.present = XIRIEvent_PR_pDUSessionModification;
                return fill(payload->event.choice.pDUSessionModification, variant_event);
            } else if constexpr (std::is_same_v<T, SmfPduSessionRelease>) {
                payload->event.present = XIRIEvent_PR_pDUSessionRelease;
                return fill(payload->event.choice.pDUSessionRelease, variant_event);
            } else if constexpr (std::is_same_v<T,
                                                SmfStartOfInterceptionWithEstablishedPduSession>) {
                payload->event.present = XIRIEvent_PR_startOfInterceptionWithEstablishedPDUSession;
                return fill(payload->event.choice.startOfInterceptionWithEstablishedPDUSession,
                            variant_event);
            } else if constexpr (std::is_same_v<T, SmfUnsuccessfulProcedure>) {
                payload->event.present = XIRIEvent_PR_unsuccessfulSMProcedure;
                return fill(payload->event.choice.unsuccessfulSMProcedure, variant_event);
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
        case XIRIEvent_PR_deregistration: {
            auto deregistration = extract(payload->event.choice.deregistration);
            if (!deregistration) {
                return tl::unexpected(deregistration.error());
            }
            out.event = *deregistration;
            return out;
        }
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
        case XIRIEvent_PR_aMFIdentifierAssociation: {
            auto assoc = extract(payload->event.choice.aMFIdentifierAssociation);
            if (!assoc) {
                return tl::unexpected(assoc.error());
            }
            out.event = *assoc;
            return out;
        }
        case XIRIEvent_PR_aMFIdentifierDeassociation: {
            auto deassoc = extract(payload->event.choice.aMFIdentifierDeassociation);
            if (!deassoc) {
                return tl::unexpected(deassoc.error());
            }
            out.event = *deassoc;
            return out;
        }
        case XIRIEvent_PR_pDUSessionEstablishment: {
            auto smf = extract(payload->event.choice.pDUSessionEstablishment);
            if (!smf) {
                return tl::unexpected(smf.error());
            }
            out.event = *smf;
            return out;
        }
        case XIRIEvent_PR_pDUSessionModification: {
            auto smf = extract(payload->event.choice.pDUSessionModification);
            if (!smf) {
                return tl::unexpected(smf.error());
            }
            out.event = *smf;
            return out;
        }
        case XIRIEvent_PR_pDUSessionRelease: {
            auto smf = extract(payload->event.choice.pDUSessionRelease);
            if (!smf) {
                return tl::unexpected(smf.error());
            }
            out.event = *smf;
            return out;
        }
        case XIRIEvent_PR_startOfInterceptionWithEstablishedPDUSession: {
            auto smf = extract(payload->event.choice.startOfInterceptionWithEstablishedPDUSession);
            if (!smf) {
                return tl::unexpected(smf.error());
            }
            out.event = *smf;
            return out;
        }
        case XIRIEvent_PR_unsuccessfulSMProcedure: {
            auto smf = extract(payload->event.choice.unsuccessfulSMProcedure);
            if (!smf) {
                return tl::unexpected(smf.error());
            }
            out.event = *smf;
            return out;
        }
        default:
            return tl::unexpected("XIRIEvent alternative " +
                                  std::to_string(static_cast<int>(payload->event.present)) +
                                  " is not modelled by li_core::xiri yet");
    }
}

} // namespace li_core::xiri
