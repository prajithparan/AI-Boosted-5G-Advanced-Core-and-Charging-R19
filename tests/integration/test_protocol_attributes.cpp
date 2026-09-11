// ADR-0346: any protocol attribute must be productisable -- 5G N40, 4G Diameter Gy, CAMEL/CAP.
//
// README recorded the gap plainly: "N40 only: Diameter Gy and CAP carry no TS 32.291 request and so
// match only unscoped offerings." An operator running 4G alongside 5G could not price a Gy-charged
// APN, a CAMEL service key, a roaming SGSN or a content Service-Identifier at all.
//
// These tests pin the property that closes it: every AVP on the wire becomes a scopable attribute,
// keyed by what the wire actually carries rather than by a name recalled from an unvendored spec.

#include "diameter_core/avp.hpp"
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
    // built against vendored material -- naming them invents nothing.
    const auto attrs = chf::cap_attributes("12", "4477000001", "4477000002", "imsi-1");
    EXPECT_EQ(attrs["CAP.serviceKey"], "12");
    EXPECT_EQ(attrs["CAP.callingPartyNumber"], "4477000001");
    EXPECT_EQ(attrs["CAP.calledPartyNumber"], "4477000002");
    EXPECT_EQ(attrs["protocol"], "CAP");
}

TEST(ProtocolAttributes, ProtocolItselfIsScopable) {
    // So an operator can price legacy traffic differently from 5G without having to find a
    // distinguishing AVP to hang the scope on.
    EXPECT_EQ(chf::gy_attributes({}, "")["chargingInformationType"], "Gy");
    EXPECT_EQ(chf::cap_attributes("", "", "", "")["chargingInformationType"], "CAP");
}
