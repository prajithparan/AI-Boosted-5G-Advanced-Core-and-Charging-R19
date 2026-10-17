// ADR-0499 phase 2: the NRF authorizes an access token request before issuing (TS 33.501
// 13.4.1.1.2 step 2; TS 29.510 5.4.2.2.1 and Table 6.3.6.3-1).
#include <map>
#include <string>

#include "token_authz.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

const std::string kSmf = "9b2a0c1e-5d6f-4a7b-8c9d-0e1f2a3b4c5d";

json smf_profile() {
    return json{{"nfInstanceId", kSmf}, {"nfType", "SMF"}, {"nfStatus", "REGISTERED"}};
}

json pcf_profile(json services) {
    json p{{"nfInstanceId", "pcf-1"}, {"nfType", "PCF"}, {"nfStatus", "REGISTERED"}};
    if (!services.is_null()) {
        p["nfServiceList"] = std::move(services);
    }
    return p;
}

class NrfTokenAuthz : public ::testing::Test {
protected:
    nrf::TokenDecision ask(std::map<std::string, std::string> extra,
                           nrf::TokenRequester cert = {.cert_cn = "smf",
                                                       .cert_dns_names = {"smf"}}) {
        std::map<std::string, std::string> f{{"grant_type", "client_credentials"},
                                             {"nfInstanceId", kSmf},
                                             {"nfType", "SMF"},
                                             {"targetNfType", "PCF"},
                                             {"scope", "npcf-smpolicycontrol"}};
        for (auto& [k, v] : extra) {
            if (v.empty()) {
                f.erase(k);
            } else {
                f[k] = v;
            }
        }
        return nrf::authorize_token_request(
            f,
            cert,
            [&](const std::string& id) -> std::optional<json> {
                return registered.contains(id) ? std::optional{registered.at(id)} : std::nullopt;
            },
            [&](const std::string& t) {
                std::vector<json> out;
                for (const auto& [id, p] : registered) {
                    if (p.at("nfType") == t) {
                        out.push_back(p);
                    }
                }
                return out;
            });
    }
    std::map<std::string, json> registered{
        {kSmf, smf_profile()},
        {"pcf-1",
         pcf_profile(json{
             {"s1", {{"serviceInstanceId", "s1"}, {"serviceName", "npcf-smpolicycontrol"}}}})}};
};

TEST_F(NrfTokenAuthz, ARegisteredConsumerWithAMatchingCertificateGetsItsToken) {
    EXPECT_EQ(ask({}).error, "");
    // The NRF's own services need no registered producer.
    EXPECT_EQ(ask({{"targetNfType", "NRF"}, {"scope", "nnrf-disc"}}).error, "");
}

TEST_F(NrfTokenAuthz, MalformedRequestsAreInvalidRequestOrUnsupportedGrantType) {
    EXPECT_EQ(ask({{"grant_type", "password"}}).error, "unsupported_grant_type");
    EXPECT_EQ(ask({{"grant_type", ""}}).error, "invalid_request");
    EXPECT_EQ(ask({{"scope", ""}}).error, "invalid_request");
    EXPECT_EQ(ask({{"nfInstanceId", ""}}).error, "invalid_request");
    // The form checks alone (the switch off) refuse the same malformed requests.
    EXPECT_EQ(nrf::check_token_request_form({{"grant_type", "password"}}).error,
              "unsupported_grant_type");
}

TEST_F(NrfTokenAuthz, AnUnregisteredOrMisdescribedConsumerIsInvalidClient) {
    EXPECT_EQ(ask({{"nfInstanceId", "not-registered"}}).error, "invalid_client");
    EXPECT_EQ(ask({{"nfType", "UDM"}}).error, "invalid_client");
}

TEST_F(NrfTokenAuthz, ACertificateNotNamingTheConsumersNfTypeIsInvalidClient) {
    // A PCF certificate claiming the SMF's registered instance.
    EXPECT_EQ(ask({}, {.cert_cn = "pcf", .cert_dns_names = {"pcf", "localhost"}}).error,
              "invalid_client");
    EXPECT_EQ(ask({}, {.cert_cn = "", .cert_dns_names = {"localhost", "SMF"}}).error, "");
}

TEST_F(NrfTokenAuthz, AServiceNoProducerOffersToTheConsumerIsInvalidScope) {
    // Not offered by any registered PCF.
    EXPECT_EQ(ask({{"scope", "npcf-policyauthorization"}}).error, "invalid_scope");
    // Every plain service name in the scope must be authorized; resource scopes are not checked.
    EXPECT_EQ(ask({{"scope", "npcf-smpolicycontrol npcf-policyauthorization"}}).error,
              "invalid_scope");
    EXPECT_EQ(ask({{"scope", "npcf-smpolicycontrol npcf-smpolicycontrol:x:y"}}).error, "");
    // No registered producer of the target type at all.
    EXPECT_EQ(ask({{"targetNfType", "UDM"}, {"scope", "nudm-sdm"}}).error, "invalid_scope");
}

TEST_F(NrfTokenAuthz, AllowedNfTypesOnTheServiceOrProfileRestrictTheConsumer) {
    registered["pcf-1"]["nfServiceList"]["s1"]["allowedNfTypes"] = json::array({"AMF"});
    EXPECT_EQ(ask({}).error, "invalid_scope");
    registered["pcf-1"]["nfServiceList"]["s1"]["allowedNfTypes"] = json::array({"AMF", "SMF"});
    EXPECT_EQ(ask({}).error, "");
    // A profile listing no services: its profile-level allowedNfTypes decide.
    registered["pcf-1"] = pcf_profile(nullptr);
    registered["pcf-1"]["allowedNfTypes"] = json::array({"AMF"});
    EXPECT_EQ(ask({}).error, "invalid_scope");
    registered["pcf-1"].erase("allowedNfTypes");
    EXPECT_EQ(ask({}).error, "");
}

TEST_F(NrfTokenAuthz, TheDeprecatedNfServicesArrayIsReadToo) {
    registered["pcf-1"] = pcf_profile(nullptr);
    registered["pcf-1"]["nfServices"] =
        json::array({{{"serviceInstanceId", "s1"}, {"serviceName", "npcf-smpolicycontrol"}}});
    EXPECT_EQ(ask({}).error, "");
    EXPECT_EQ(ask({{"scope", "npcf-am-policy-control"}}).error, "invalid_scope");
}

TEST_F(NrfTokenAuthz, TheErrorBodyIsAnAccessTokenErr) {
    const auto body = nrf::access_token_err(ask({{"nfType", "UDM"}}));
    EXPECT_EQ(body.at("error"), "invalid_client");
    EXPECT_TRUE(body.at("error_description").is_string());
}

} // namespace
