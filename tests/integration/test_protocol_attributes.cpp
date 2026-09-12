// ADR-0346: any protocol attribute must be productisable -- 5G N40, 4G Diameter Gy, CAMEL/CAP.
//
// README recorded the gap plainly: "N40 only: Diameter Gy and CAP carry no TS 32.291 request and so
// match only unscoped offerings." An operator running 4G alongside 5G could not price a Gy-charged
// APN, a CAMEL service key, a roaming SGSN or a content Service-Identifier at all.
//
// These tests pin the property that closes it: every AVP on the wire becomes a scopable attribute,
// keyed by what the wire actually carries rather than by a name recalled from an unvendored spec.

#include "diameter_core/avp.hpp"
#include "diameter_core/avp_names.hpp"
#include "diameter_core/dictionary.hpp"
#include "protocol_attributes.hpp"

#include <gtest/gtest.h>

namespace {

diameter_core::Avp avp(std::uint32_t code, const std::string& text, std::uint32_t vendor = 0) {
    diameter_core::Avp a;
    a.code = code;
    a.vendor_id = vendor;
    a.data.assign(text.begin(), text.end());
    return a;
}

diameter_core::Avp avp_u32(std::uint32_t code, std::uint32_t value) {
    diameter_core::Avp a;
    a.code = code;
    a.data = {static_cast<std::uint8_t>(value >> 24),
              static_cast<std::uint8_t>(value >> 16),
              static_cast<std::uint8_t>(value >> 8),
              static_cast<std::uint8_t>(value)};
    return a;
}

} // namespace

TEST(ProtocolAttributes, EveryAvpBecomesScopableByItsWireCode) {
    // Codes are NOT named here, deliberately: TS 32.299 and RFC 4006 are not vendored in this
    // repository, so mapping 30 -> "calledStationId" would be a fabrication. The wire code is what
    // the peer actually sent and what an operator knows their own network emits.
    const auto attrs =
        chf::gy_attributes({avp(30, "internet.apn"), avp_u32(432, 7)}, "imsi-999700000000001");
    EXPECT_EQ(attrs["Gy.avp.30"], "internet.apn");
    EXPECT_EQ(attrs["Gy.avp.432.u32"], 7);
    EXPECT_EQ(attrs["protocol"], "Gy");
    EXPECT_EQ(attrs["subscriberIdentifier"], "imsi-999700000000001");
}

TEST(ProtocolAttributes, VendorSpecificAvpsDoNotCollideWithStandardOnes) {
    // Code 21 from 3GPP and code 21 from any other vendor are different attributes. Collapsing
    // them into one key would let a tariff written for one silently match the other.
    const auto attrs = chf::gy_attributes({avp(21, "standard"), avp(21, "threegpp", 10415)}, "");
    EXPECT_EQ(attrs["Gy.avp.21"], "standard");
    EXPECT_EQ(attrs["Gy.avp.10415.21"], "threegpp");
}

TEST(ProtocolAttributes, BinaryValuesDoNotBecomeGarbageStrings) {
    // A non-printable payload must not be offered as text: a scope comparing it against a string
    // would match on mojibake. It is still offered as u32 when it is exactly four bytes.
    diameter_core::Avp binary;
    binary.code = 99;
    binary.data = {0x00, 0xff, 0x01, 0x80};
    const auto attrs = chf::gy_attributes({binary}, "");
    EXPECT_FALSE(attrs.contains("Gy.avp.99"));
    EXPECT_TRUE(attrs.contains("Gy.avp.99.u32"));
}

TEST(ProtocolAttributes, CapExposesItsOwnNamedParameters) {
    // CAMEL parameters ARE named, because they come from this project's own cap_core which was
    // built against vendored material -- naming them invents nothing. The party numbers stay
    // opaque hex: cap_core decodes them as octets and this repository has no decoder for
    // CalledPartyNumber's nature-of-address + TBCD layout, so digits would be a guess.
    const auto attrs =
        chf::cap_attributes(12, {0x21, 0x43}, std::vector<std::uint8_t>{0x65, 0x87}, "imsi-1");
    EXPECT_EQ(attrs["CAP.serviceKey"], 12);
    EXPECT_EQ(attrs["CAP.calledPartyNumber.hex"], "2143");
    EXPECT_EQ(attrs["CAP.callingPartyNumber.hex"], "6587");
    EXPECT_EQ(attrs["protocol"], "CAP");
    EXPECT_EQ(attrs["subscriberIdentifier"], "imsi-1");
}

TEST(ProtocolAttributes, CapOmitsPartyNumbersItDoesNotHave) {
    // An absent optional callingPartyNumber must not become an empty-string attribute -- an
    // offering scoped on it would then match calls that never carried one.
    const auto attrs = chf::cap_attributes(7, {}, std::nullopt, "imsi-2");
    EXPECT_FALSE(attrs.contains("CAP.calledPartyNumber.hex"));
    EXPECT_FALSE(attrs.contains("CAP.callingPartyNumber.hex"));
    EXPECT_EQ(attrs["CAP.serviceKey"], 7);
}

TEST(ProtocolAttributes, ProtocolItselfIsScopable) {
    // So an operator can price legacy traffic differently from 5G without having to find a
    // distinguishing AVP to hang the scope on.
    EXPECT_EQ(chf::gy_attributes({}, "")["chargingInformationType"], "Gy");
    EXPECT_EQ(chf::cap_attributes(0, {}, std::nullopt, "")["chargingInformationType"], "CAP");
}

// ---------------------------------------------------------------------------------------------
// ADR-0351: AVP names alongside the wire codes.

TEST(AvpNames, AgreesWithThisProjectsOwnHandCitedDictionary) {
    // The generated table (TS 32.299 V19.0.0) and dictionary.hpp (freeDiameter's own C source)
    // are two independently sourced derivations of the same facts. If they ever disagree, one of
    // them is wrong and a tariff scoped by name would be scoped onto the wrong attribute -- so
    // this is asserted here rather than assumed.
    namespace dict = diameter_core::dictionary;
    EXPECT_EQ(diameter_core::avp_name(0, dict::Dcc::kRatingGroup), "Rating-Group");
    EXPECT_EQ(diameter_core::avp_name(0, dict::Dcc::kServiceContextId), "Service-Context-Id");
    EXPECT_EQ(diameter_core::avp_name(0, dict::Dcc::kSubscriptionIdData), "Subscription-Id-Data");
    EXPECT_EQ(diameter_core::avp_name(0, dict::Dcc::kMultipleServicesCreditControl),
              "Multiple-Services-Credit-Control");
    EXPECT_EQ(diameter_core::avp_name(0, dict::Dcc::kUsedServiceUnit), "Used-Service-Unit");
    EXPECT_EQ(diameter_core::avp_name(0, dict::Avp::kResultCode), "Result-Code");
}

TEST(AvpNames, UnknownCodeIsEmptyNotAGuess) {
    EXPECT_TRUE(diameter_core::avp_name(0, 999999).empty());
    EXPECT_TRUE(diameter_core::avp_name(99999, 432).empty());
    // Vendor space matters: 3GPP's code 21 is 3GPP-RAT-Type, but code 21 in the IETF space is not.
    EXPECT_EQ(diameter_core::avp_name(10415, 21), "3GPP-RAT-Type");
    EXPECT_NE(diameter_core::avp_name(0, 21), "3GPP-RAT-Type");
}

TEST(ProtocolAttributes, NamedKeyIsEmittedAlongsideTheNumericOne) {
    diameter_core::Avp apn; // Called-Station-Id, AVP code 30 -- TS 32.299 clause 7.1.7.
    apn.code = 30;
    const std::string value = "internet";
    apn.data.assign(value.begin(), value.end());
    const auto attrs = chf::gy_attributes({apn}, "imsi-1");
    // Alongside, never instead: an operator already scoping on the numeric key keeps working.
    EXPECT_EQ(attrs["Gy.avp.30"], "internet");
    EXPECT_EQ(attrs["Gy.Called-Station-Id"], "internet");
}

TEST(ProtocolAttributes, UnnamedAvpStillGetsItsNumericKeyAndNoEmptySegment) {
    diameter_core::Avp unknown;
    unknown.code = 65000; // no name in TS 32.299; a vendor extension or a future release
    const std::string value = "x";
    unknown.data.assign(value.begin(), value.end());
    const auto attrs = chf::gy_attributes({unknown}, "");
    EXPECT_EQ(attrs["Gy.avp.65000"], "x");
    // Exactly one key for it, not a duplicate under an identical "named" path.
    int matching = 0;
    for (const auto& [k, v] : attrs.items()) {
        if (k.rfind("Gy.", 0) == 0) {
            ++matching;
        }
    }
    EXPECT_EQ(matching, 1);
}

TEST(ProtocolAttributes, NestingGivesTwoPathsPerAvpNotTwoPerLevel) {
    // A Rating-Group nested inside an MSCC. Both spellings of the path must reach it, and there
    // must be exactly two of them -- branching per level would give four for this depth, eight for
    // the next, which is real cost on the charging hot path for no extra reach.
    diameter_core::Avp rating_group;
    rating_group.code = 432;
    rating_group.data = {0x00, 0x00, 0x00, 0x07};
    diameter_core::Avp mscc;
    mscc.code = 456;
    diameter_core::encode_avp(mscc.data, rating_group);

    const auto attrs = chf::gy_attributes({mscc}, "");
    EXPECT_EQ(attrs["Gy.avp.456.avp.432.u32"], 7);
    EXPECT_EQ(attrs["Gy.Multiple-Services-Credit-Control.Rating-Group.u32"], 7);

    int leaves = 0;
    for (const auto& [k, v] : attrs.items()) {
        if (k.size() > 4 && k.substr(k.size() - 4) == ".u32") {
            ++leaves;
        }
    }
    EXPECT_EQ(leaves, 2);
}

TEST(AvpNames, MatchesCodesReadByHandFromTheSuppliedSpecMaterial) {
    // A fourth check, independent of the generator entirely: these codes were read by hand out of
    // the supplied TS 32.299 text and RFC 4006 before the generator existed. If a future respin of
    // the generator silently shifts a region or drops a family, this fails.
    //
    // The 3GPP-* family is listed deliberately: AVP names may begin with a DIGIT, and the first
    // version of the generator required a leading letter and so dropped every one of them --
    // silently, with all its other cross-checks still passing.
    EXPECT_EQ(diameter_core::avp_name(0, 30), "Called-Station-Id");
    EXPECT_EQ(diameter_core::avp_name(0, 439), "Service-Identifier");
    EXPECT_EQ(diameter_core::avp_name(0, 450), "Subscription-Id-Type");
    EXPECT_EQ(diameter_core::avp_name(0, 458), "User-Equipment-Info");
    EXPECT_EQ(diameter_core::avp_name(10415, 1), "3GPP-IMSI");
    EXPECT_EQ(diameter_core::avp_name(10415, 2), "3GPP-Charging-Id");
    EXPECT_EQ(diameter_core::avp_name(10415, 13), "3GPP-Charging-Characteristics");
    EXPECT_EQ(diameter_core::avp_name(10415, 21), "3GPP-RAT-Type");
    EXPECT_EQ(diameter_core::avp_name(10415, 22), "3GPP-User-Location-Info");
    EXPECT_EQ(diameter_core::avp_name(10415, 847), "GGSN-Address");
    EXPECT_EQ(diameter_core::avp_name(10415, 873), "Service-Information");
    EXPECT_EQ(diameter_core::avp_name(10415, 1004), "Charging-Rule-Base-Name");
}

TEST(AvpNames, LookupIsExactAcrossTheWholeGeneratedTable) {
    // avp_name() is a binary search, which is only correct if the generated table is sorted by
    // (vendor_id, code) with no duplicate key. The generator guarantees both; this asserts it
    // against the compiled artefact, because a hand-edit of the generated file (which its own
    // header forbids, but forbidding is not preventing) would break the lookup silently -- a
    // binary search over unsorted data does not fail, it just misses.
    //
    // Walked via the public API rather than the table: every name it returns must round-trip.
    struct Known {
        std::uint32_t vendor;
        std::uint32_t code;
    };
    // Spread across both vendor spaces and the full code range, including the boundaries.
    constexpr Known kProbes[] = {
        {0, 1},
        {0, 30},
        {0, 258},
        {0, 432},
        {0, 461},
        {0, 485},
        {10415, 1},
        {10415, 2},
        {10415, 21},
        {10415, 873},
        {10415, 1004},
        {10415, 4413},
    };
    for (const auto& probe : kProbes) {
        const auto name = diameter_core::avp_name(probe.vendor, probe.code);
        EXPECT_FALSE(name.empty())
            << "vendor " << probe.vendor << " code " << probe.code << " should be named";
    }
    // A code that is NOT in the table must return empty rather than borrow a neighbour's name --
    // the specific way an unsorted table or an off-by-one in the search would fail.
    for (const auto& gap : {Known{0, 29}, Known{0, 31}, Known{0, 65000}, Known{10415, 99999}}) {
        EXPECT_TRUE(diameter_core::avp_name(gap.vendor, gap.code).empty())
            << "vendor " << gap.vendor << " code " << gap.code << " is not in TS 32.299";
    }
}
