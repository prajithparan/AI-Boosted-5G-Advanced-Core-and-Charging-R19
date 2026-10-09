// ADR-0477: NRF discovery authorization (TS 33.501 13.3.1.3; TS 29.510 6.1.6.2.2/6.1.6.2.3).
#include "discovery_authz.hpp"

#include <gtest/gtest.h>

using nlohmann::json;
using nrf::DiscoveryRequester;
using nrf::profile_discoverable_by;

namespace {
DiscoveryRequester req(const char* type, json plmns = nullptr, json snssais = nullptr) {
    DiscoveryRequester r;
    if (type != nullptr) {
        r.nf_type = type;
    }
    r.plmns = std::move(plmns);
    r.snssais = std::move(snssais);
    return r;
}
} // namespace

TEST(NrfDiscoveryAuthz, NoAllowedListsMeansAnyRequester) {
    const json p = {{"nfType", "UDM"}};
    EXPECT_TRUE(profile_discoverable_by(p, req("AMF")));
    EXPECT_TRUE(profile_discoverable_by(p, req(nullptr)));
}

TEST(NrfDiscoveryAuthz, AllowedNfTypesRestrictsAndMissingRequesterTypeIsRefused) {
    const json p = {{"allowedNfTypes", {"AMF", "SMF"}}};
    EXPECT_TRUE(profile_discoverable_by(p, req("SMF")));
    EXPECT_FALSE(profile_discoverable_by(p, req("PCF")));
    EXPECT_FALSE(profile_discoverable_by(p, req(nullptr)));
}

TEST(NrfDiscoveryAuthz, AllowedPlmnsAndProfilePlmnListIsImplicitlyAllowed) {
    const json home = {{"mcc", "999"}, {"mnc", "70"}};
    const json other = {{"mcc", "310"}, {"mnc", "260"}};
    const json p = {{"plmnList", {home}}, {"allowedPlmns", {other}}};
    EXPECT_TRUE(profile_discoverable_by(p, req("AMF", json::array({other}))));
    EXPECT_TRUE(profile_discoverable_by(p, req("AMF", json::array({home}))));
    EXPECT_FALSE(
        profile_discoverable_by(p, req("AMF", json::array({json{{"mcc", "001"}, {"mnc", "01"}}}))));
    EXPECT_FALSE(profile_discoverable_by(p, req("AMF")));
}

TEST(NrfDiscoveryAuthz, AllowedNssaisRequiresSstAndSdIdentical) {
    const json p = {{"allowedNssais", {json{{"sst", 1}, {"sd", "000001"}}}}};
    EXPECT_TRUE(profile_discoverable_by(
        p, req("AMF", nullptr, json::array({json{{"sst", 1}, {"sd", "000001"}}}))));
    EXPECT_FALSE(profile_discoverable_by(p, req("AMF", nullptr, json::array({json{{"sst", 1}}}))));
    EXPECT_FALSE(profile_discoverable_by(
        p, req("AMF", nullptr, json::array({json{{"sst", 1}, {"sd", "000002"}}}))));
    EXPECT_FALSE(profile_discoverable_by(p, req("AMF")));
}

TEST(NrfDiscoveryAuthz, ServiceListPrevailsAndAnyServiceSuffices) {
    const json p = {
        {"allowedNfTypes", {"AMF"}},
        {"nfServices",
         {json{{"serviceName", "a"}, {"allowedNfTypes", {"PCF"}}}, json{{"serviceName", "b"}}}}};
    // PCF: service a allows it (service list prevails over the profile's AMF-only list).
    EXPECT_TRUE(profile_discoverable_by(p, req("PCF")));
    // AMF: service b has no own list, inherits the profile's {AMF}.
    EXPECT_TRUE(profile_discoverable_by(p, req("AMF")));
    EXPECT_FALSE(profile_discoverable_by(p, req("SMF")));
}
