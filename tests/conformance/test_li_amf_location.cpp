// AMF NGAP UserLocationInformation -> li_core::xiri::UserLocation (LI increment 4, prerequisite 1).
// Builds real NGAP UserLocationInformation values (NR and E-UTRA) with hand-encoded TBCD PLMNs and
// left-aligned BIT STRING cell identities, and asserts parse_user_location decodes them to the xiri
// Location the LocationUpdate / IdentifierAssociation xIRIs carry. Links li_core + ngap_generated,
// the coexistence shape the AMF has (test_li_ngap_coexistence.cpp proves the two libraries link).

#include <EUTRA-CGI.h>
#include <NR-CGI.h>
#include <TAI.h>
#include <UserLocationInformation.h>
#include <UserLocationInformationEUTRA.h>
#include <UserLocationInformationNR.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "li_location.hpp"

#include <gtest/gtest.h>

namespace {

// TBCD PLMN, matching ngap_task.cpp encode_plmn_identity. mcc="001", mnc="01" -> {0x00,0xF1,0x10}.
void set_plmn(OCTET_STRING_t& os, const char* mcc, const char* mnc) {
    const int mcc1 = mcc[0] - '0', mcc2 = mcc[1] - '0', mcc3 = mcc[2] - '0';
    const bool three = std::strlen(mnc) == 3;
    const int mnc1 = mnc[0] - '0', mnc2 = mnc[1] - '0', mnc3 = three ? (mnc[2] - '0') : 0xF;
    std::uint8_t b[3] = {static_cast<std::uint8_t>((mcc2 << 4) | mcc1),
                         static_cast<std::uint8_t>((mnc3 << 4) | mcc3),
                         static_cast<std::uint8_t>((mnc2 << 4) | mnc1)};
    OCTET_STRING_fromBuf(&os, reinterpret_cast<const char*>(b), 3);
}

void set_octets(OCTET_STRING_t& os, std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::uint8_t> v(bytes);
    OCTET_STRING_fromBuf(&os, reinterpret_cast<const char*>(v.data()), static_cast<int>(v.size()));
}

// Left-aligned BIT STRING: value occupies the top `bits`, low (size*8 - bits) bits are padding.
void set_cell_id(BIT_STRING_t& bs, std::uint64_t value, unsigned bits) {
    const unsigned nbytes = (bits + 7) / 8;
    const unsigned unused = nbytes * 8 - bits;
    const std::uint64_t shifted = value << unused;
    bs.buf = static_cast<std::uint8_t*>(std::calloc(nbytes, 1));
    bs.size = nbytes;
    bs.bits_unused = static_cast<int>(unused);
    for (unsigned i = 0; i < nbytes; ++i) {
        bs.buf[nbytes - 1 - i] = static_cast<std::uint8_t>((shifted >> (8 * i)) & 0xFF);
    }
}

} // namespace

TEST(LiAmfLocation, DecodesNrUserLocation) {
    UserLocationInformation uli{};
    uli.present = UserLocationInformation_PR_userLocationInformationNR;
    auto* nr =
        static_cast<UserLocationInformationNR*>(std::calloc(1, sizeof(UserLocationInformationNR)));
    set_plmn(nr->nR_CGI.pLMNIdentity, "001", "01");
    set_cell_id(nr->nR_CGI.nRCellIdentity, 0x123456789ULL, 36);
    set_plmn(nr->tAI.pLMNIdentity, "001", "01");
    set_octets(nr->tAI.tAC, {0x00, 0x00, 0x2A});
    uli.choice.userLocationInformationNR = nr;

    const auto loc = amf::parse_user_location(uli);
    ASSERT_TRUE(loc.has_value());
    ASSERT_TRUE(loc->nr.has_value());
    EXPECT_FALSE(loc->eutra.has_value());
    EXPECT_EQ(loc->nr->ncgi.plmn.mcc, "001");
    EXPECT_EQ(loc->nr->ncgi.plmn.mnc, "01");
    EXPECT_EQ(loc->nr->ncgi.nr_cell_id, 0x123456789ULL);
    EXPECT_EQ(loc->nr->tai.plmn.mcc, "001");
    ASSERT_EQ(loc->nr->tai.tac.size(), 3u);
    EXPECT_EQ(loc->nr->tai.tac[2], 0x2A);

    ASN_STRUCT_FREE(asn_DEF_UserLocationInformationNR, nr);
}

TEST(LiAmfLocation, DecodesEutraUserLocationWith3DigitMnc) {
    UserLocationInformation uli{};
    uli.present = UserLocationInformation_PR_userLocationInformationEUTRA;
    auto* eutra = static_cast<UserLocationInformationEUTRA*>(
        std::calloc(1, sizeof(UserLocationInformationEUTRA)));
    set_plmn(eutra->eUTRA_CGI.pLMNIdentity, "310", "260"); // 3-digit MNC
    set_cell_id(eutra->eUTRA_CGI.eUTRACellIdentity, 0x0ABCDEFULL, 28);
    set_plmn(eutra->tAI.pLMNIdentity, "310", "260");
    set_octets(eutra->tAI.tAC, {0x12, 0x34});
    uli.choice.userLocationInformationEUTRA = eutra;

    const auto loc = amf::parse_user_location(uli);
    ASSERT_TRUE(loc.has_value());
    ASSERT_TRUE(loc->eutra.has_value());
    EXPECT_FALSE(loc->nr.has_value());
    EXPECT_EQ(loc->eutra->ecgi.plmn.mcc, "310");
    EXPECT_EQ(loc->eutra->ecgi.plmn.mnc, "260");
    EXPECT_EQ(loc->eutra->ecgi.eutra_cell_id, 0x0ABCDEFULL);
    ASSERT_EQ(loc->eutra->tai.tac.size(), 2u);

    ASN_STRUCT_FREE(asn_DEF_UserLocationInformationEUTRA, eutra);
}

TEST(LiAmfLocation, UnmodelledBranchReturnsNullopt) {
    UserLocationInformation uli{};
    uli.present = UserLocationInformation_PR_userLocationInformationN3IWF;
    uli.choice.userLocationInformationN3IWF = nullptr;
    EXPECT_FALSE(amf::parse_user_location(uli).has_value());
}
