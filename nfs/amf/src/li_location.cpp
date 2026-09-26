#include "li_location.hpp"

#include <EUTRA-CGI.h>
#include <NR-CGI.h>
#include <TAI.h>
#include <UserLocationInformation.h>
#include <UserLocationInformationEUTRA.h>
#include <UserLocationInformationNR.h>
#include <cstdint>
#include <ngap_core/ngap_codec.hpp>
#include <string>
#include <vector>

namespace amf {
namespace {

// PLMNIdentity is the TS 23.003 TBCD-coded triple, the exact inverse of ngap_task.cpp's
// encode_plmn_identity: octet 0 = (MCC2<<4)|MCC1, octet 1 = (MNC3<<4)|MCC3, octet 2 =
// (MNC2<<4)|MNC1, with MNC3 == 0xF marking a two-digit MNC.
li_core::xiri::Plmnid decode_plmn(const OCTET_STRING_t& o) {
    li_core::xiri::Plmnid p;
    if (o.buf == nullptr || o.size < 3) {
        return p;
    }
    const auto b0 = o.buf[0];
    const auto b1 = o.buf[1];
    const auto b2 = o.buf[2];
    const int mcc1 = b0 & 0x0F;
    const int mcc2 = (b0 >> 4) & 0x0F;
    const int mcc3 = b1 & 0x0F;
    const int mnc3 = (b1 >> 4) & 0x0F;
    const int mnc1 = b2 & 0x0F;
    const int mnc2 = (b2 >> 4) & 0x0F;
    const auto digit = [](int d) { return static_cast<char>('0' + d); };
    p.mcc = std::string{digit(mcc1), digit(mcc2), digit(mcc3)};
    p.mnc = (mnc3 == 0x0F) ? std::string{digit(mnc1), digit(mnc2)}
                           : std::string{digit(mnc1), digit(mnc2), digit(mnc3)};
    return p;
}

// A BIT STRING cell identity (NR 36-bit / E-UTRA 28-bit) is stored left-aligned, MSB first, with
// the last octet's low bits_unused bits as padding -- the inverse of xiri.cpp's set_bit_string.
// Accumulate every octet, then drop the trailing padding.
std::uint64_t decode_cell_id(const BIT_STRING_t& bs) {
    if (bs.buf == nullptr || bs.size == 0) {
        return 0;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < bs.size; ++i) {
        v = (v << 8) | bs.buf[i];
    }
    const int unused = (bs.bits_unused >= 0 && bs.bits_unused <= 7) ? bs.bits_unused : 0;
    return v >> static_cast<unsigned>(unused);
}

std::vector<std::uint8_t> to_octets(const OCTET_STRING_t& o) {
    if (o.buf == nullptr || o.size == 0) {
        return {};
    }
    return std::vector<std::uint8_t>(o.buf, o.buf + o.size);
}

} // namespace

std::optional<li_core::xiri::UserLocation> parse_user_location(const UserLocationInformation& uli) {
    using namespace li_core::xiri;
    switch (uli.present) {
        case UserLocationInformation_PR_userLocationInformationNR: {
            const auto* nr = uli.choice.userLocationInformationNR;
            if (nr == nullptr) {
                return std::nullopt;
            }
            NrLocation loc;
            loc.tai.plmn = decode_plmn(nr->tAI.pLMNIdentity);
            loc.tai.tac = to_octets(nr->tAI.tAC);
            loc.ncgi.plmn = decode_plmn(nr->nR_CGI.pLMNIdentity);
            loc.ncgi.nr_cell_id = decode_cell_id(nr->nR_CGI.nRCellIdentity);
            UserLocation ul;
            ul.nr = loc;
            return ul;
        }
        case UserLocationInformation_PR_userLocationInformationEUTRA: {
            const auto* eutra = uli.choice.userLocationInformationEUTRA;
            if (eutra == nullptr) {
                return std::nullopt;
            }
            EutraLocation loc;
            loc.tai.plmn = decode_plmn(eutra->tAI.pLMNIdentity);
            loc.tai.tac = to_octets(eutra->tAI.tAC);
            loc.ecgi.plmn = decode_plmn(eutra->eUTRA_CGI.pLMNIdentity);
            loc.ecgi.eutra_cell_id =
                static_cast<std::uint32_t>(decode_cell_id(eutra->eUTRA_CGI.eUTRACellIdentity));
            UserLocation ul;
            ul.eutra = loc;
            return ul;
        }
        default:
            // userLocationInformationN3IWF and the choice extensions are not modelled -- report no
            // location rather than a fabricated one.
            return std::nullopt;
    }
}

std::optional<li_core::xiri::UserLocation>
user_location_from_ies(const ConcreteProtocolIE_Container& container) {
    const auto* ie = ::ngap::find_ie(container, 121 /* id-UserLocationInformation */);
    if (ie == nullptr) {
        return std::nullopt;
    }
    auto* uli = static_cast<UserLocationInformation_t*>(
        ::ngap::decode_ie_value(&asn_DEF_UserLocationInformation, *ie));
    if (uli == nullptr) {
        return std::nullopt;
    }
    auto out = parse_user_location(*uli);
    ASN_STRUCT_FREE(asn_DEF_UserLocationInformation, uli);
    return out;
}

} // namespace amf
