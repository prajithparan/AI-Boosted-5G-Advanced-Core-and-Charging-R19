// ADR-0499 phase 1: the sbi-core producer-side authorizer against TS 29.500 6.7.3
// (TS_29.500_j70.txt:3066-3085) and TS29510_Nnrf_AccessToken.yaml AccessTokenClaims.
#include "sbi_core/api_catalog.hpp"
#include "sbi_core/authorizer.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {

using sbi_core::http2::Authorizer;
using sbi_core::http2::AuthorizerConfig;
using sbi_core::http2::Request;
using Traits = jwt::traits::nlohmann_json;

constexpr const char* kNrfId = "6fa459ea-ee8a-3ca4-894e-db77e160355e";
constexpr const char* kPcfId = "8c5b3a3e-1f0e-4c3a-9d6e-2b1f6a7c9d10";

std::string pem(EVP_PKEY* key, bool is_private) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (is_private) {
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr);
    } else {
        PEM_write_bio_PUBKEY(bio.get(), key);
    }
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);
    return {data, static_cast<std::size_t>(len)};
}

class SbiAuthorizer : public ::testing::Test {
protected:
    void SetUp() override {
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen("P-256"), EVP_PKEY_free);
        ASSERT_NE(key, nullptr);
        private_pem_ = pem(key.get(), true);
        pub_path_ =
            std::filesystem::temp_directory_path() /
            ("authz_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".pem");
        std::ofstream(pub_path_) << pem(key.get(), false);
        auto verifier = std::make_shared<sbi_core::jwt::Verifier>(pub_path_.string(), kNrfId);
        authorizer_ = std::make_unique<Authorizer>(
            AuthorizerConfig{.required = true, .nf_type = "PCF", .nf_instance_id = kPcfId},
            verifier);
        optional_ = std::make_unique<Authorizer>(
            AuthorizerConfig{.required = false, .nf_type = "PCF", .nf_instance_id = kPcfId},
            verifier);
    }
    void TearDown() override { std::filesystem::remove(pub_path_); }

    // A token with every AccessTokenClaims required claim; tests drop or change one.
    jwt::builder<jwt::default_clock, Traits> claims() const {
        const auto now = std::chrono::system_clock::now();
        return jwt::create<Traits>()
            .set_issuer(kNrfId)
            .set_subject("1d2c3b4a-0000-4000-8000-000000000001")
            .set_audience("PCF")
            .set_expires_at(now + std::chrono::minutes(5))
            .set_payload_claim("scope",
                               jwt::basic_claim<Traits>(std::string("npcf-am-policy-control")));
    }
    std::string sign(jwt::builder<jwt::default_clock, Traits> b) const {
        return b.sign(jwt::algorithm::es256("", private_pem_, "", ""));
    }
    static Request request(std::string path, std::optional<std::string> authorization) {
        Request r;
        r.method = "POST";
        r.path = std::move(path);
        r.authority = "pcf.example:8443";
        if (authorization) {
            r.headers.emplace("authorization", *authorization);
        }
        return r;
    }
    static std::string www_authenticate(const sbi_core::http2::Response& r) {
        const auto it = r.headers.find("www-authenticate");
        return it == r.headers.end() ? "" : it->second;
    }

    std::string private_pem_;
    std::filesystem::path pub_path_;
    std::unique_ptr<Authorizer> authorizer_;
    std::unique_ptr<Authorizer> optional_;
};

constexpr const char* kAmPath = "/npcf-am-policy-control/v1/policies";

TEST_F(SbiAuthorizer, AValidTokenForThisNfAndApiIsAuthorized) {
    EXPECT_FALSE(authorizer_->check(request(kAmPath, "Bearer " + sign(claims()))).has_value());
    // RFC 6750 2.1 / RFC 9110 11.1: the scheme is case-insensitive.
    EXPECT_FALSE(authorizer_->check(request(kAmPath, "bearer " + sign(claims()))).has_value());
}

TEST_F(SbiAuthorizer, NoTokenIs401WithBearerRealmAndNoErrorAttribute) {
    const auto r = authorizer_->check(request(kAmPath, std::nullopt));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(www_authenticate(*r),
              R"(Bearer realm="https://pcf.example:8443/npcf-am-policy-control/v1")");
}

TEST_F(SbiAuthorizer, NoTokenIsAcceptedOnlyWhenLocalConfigurationAllowsIt) {
    EXPECT_FALSE(optional_->check(request(kAmPath, std::nullopt)).has_value());
    // A token that is sent is still validated (TS 29.500 6.7.3 first paragraph).
    const auto r = optional_->check(request(kAmPath, "Bearer not-a-jwt"));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 401);
}

TEST_F(SbiAuthorizer, AnInvalidTokenIs401InvalidToken) {
    for (const std::string& auth :
         {std::string("Bearer not-a-jwt"),
          std::string("Basic dXNlcjpwYXNz"),
          "Bearer " + sign(claims().set_issuer("someone-else")),
          "Bearer " + sign(claims().set_expires_at(std::chrono::system_clock::now() -
                                                   std::chrono::minutes(1)))}) {
        const auto r = authorizer_->check(request(kAmPath, auth));
        ASSERT_TRUE(r.has_value()) << auth;
        EXPECT_EQ(r->status, 401) << auth;
        EXPECT_NE(www_authenticate(*r).find(R"(error="invalid_token")"), std::string::npos) << auth;
    }
}

TEST_F(SbiAuthorizer, AMissingRequiredClaimIs401AccessTokenClaimMissing) {
    auto b = jwt::create<Traits>().set_issuer(kNrfId).set_audience("PCF").set_payload_claim(
        "scope", jwt::basic_claim<Traits>(std::string("npcf-am-policy-control")));
    const auto r = authorizer_->check(request(kAmPath, "Bearer " + sign(b)));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 401);
    const auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body.at("cause"), "ACCESS_TOKEN_CLAIM_MISSING");
    std::vector<std::string> params;
    for (const auto& p : body.at("invalidParams")) {
        params.push_back(p.at("param"));
    }
    EXPECT_EQ(params, (std::vector<std::string>{"sub", "exp"}));
}

TEST_F(SbiAuthorizer, ATokenForAnotherNfIs401InvalidToken) {
    const auto r =
        authorizer_->check(request(kAmPath, "Bearer " + sign(claims().set_audience("UDM"))));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 401);
    EXPECT_NE(www_authenticate(*r).find(R"(error="invalid_token")"), std::string::npos);
}

TEST_F(SbiAuthorizer, AnAudienceListNamingThisInstanceIsAccepted) {
    // AccessTokenClaims.aud: array of NfInstanceId form.
    const auto token = sign(claims().set_audience(nlohmann::json::array_t{"other-id", kPcfId}));
    EXPECT_FALSE(authorizer_->check(request(kAmPath, "Bearer " + token)).has_value());
}

TEST_F(SbiAuthorizer, AScopeWithoutTheApisServiceNameIs403InsufficientScope) {
    const auto token = sign(claims().set_payload_claim(
        "scope",
        jwt::basic_claim<Traits>(std::string("npcf-smpolicycontrol npcf-am-policy-control:x"))));
    const auto r = authorizer_->check(request(kAmPath, "Bearer " + token));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 403);
    EXPECT_EQ(www_authenticate(*r),
              R"(Bearer realm="https://pcf.example:8443/npcf-am-policy-control/v1", )"
              R"(error="insufficient_scope", scope="npcf-am-policy-control")");
}

TEST_F(SbiAuthorizer, TheRequiredScopeComesFromTheApisYaml) {
    // TS29541_Nnef_SMService.yaml: apiName nnef-smservice, scope nnef-smcontext.
    EXPECT_EQ(Authorizer::required_scope("/nnef-smservice/v1/sm-contexts"), "nnef-smcontext");
    EXPECT_EQ(Authorizer::required_scope("/nudm-sdm/v2/imsi-001/am-data?x=1"), "nudm-sdm");
    // Not a 3GPP API (no YAML): its apiName.
    EXPECT_EQ(Authorizer::required_scope("/udr-oam/v1/x"), "udr-oam");
    EXPECT_EQ(Authorizer::required_scope("/"), "");
}

TEST_F(SbiAuthorizer, TheRealmCannotBreakOutOfItsQuotedString) {
    auto req = request(kAmPath, std::nullopt);
    req.authority = "evil\", error=\"x";
    const auto r = authorizer_->check(req);
    ASSERT_TRUE(r.has_value());
    // The injected quotes are dropped: the header stays one quoted realm, no error attribute.
    const auto h = www_authenticate(*r);
    EXPECT_EQ(std::ranges::count(h, '"'), 2) << h;
}

TEST(SbiApiCatalog, NamesAndProducersComeFromTheYamls) {
    using sbi_core::api_catalog::api_name_of;
    using sbi_core::api_catalog::find;
    EXPECT_EQ(api_name_of("https://udm.example:8443/nudm-sdm/v2/imsi-1/am-data?x=1"), "nudm-sdm");
    EXPECT_EQ(api_name_of("/nudm-sdm/v2"), "nudm-sdm");
    EXPECT_EQ(api_name_of("https://nrf.example/"), "");
    ASSERT_TRUE(find("nudm-sdm").has_value());
    EXPECT_EQ(find("nudm-sdm")->nf_type, "UDM"); // TS29503_Nudm_SDM.yaml
    EXPECT_EQ(find("n5g-eir-eic")->nf_type,
              "5G_EIR"); // TS29511_N5g-eir_EquipmentIdentityCheck.yaml
    EXPECT_EQ(find("nnef-smservice")->scope, "nnef-smcontext");
    EXPECT_EQ(find("3gpp-akma")->nf_type, "");         // TS29522_AKMA.yaml: no N<nf> service name
    EXPECT_EQ(find("nmbsmf-tmgi")->nf_type, "MB_SMF"); // TS29532_Nmbsmf_TMGI.yaml, "_" dropped
    EXPECT_EQ(find("nrouter-smservice")->nf_type, ""); // SMS_ROUTER only by prefix: not guessed
    EXPECT_FALSE(find("udr-oam").has_value());
    // TS29522_TrafficInfluence.yaml declares oAuth2ClientCredentials with `scopes: {}`: a token is
    // required, no scope is checked (ADR-0499).
    ASSERT_TRUE(find("3gpp-traffic-influence").has_value());
    EXPECT_EQ(find("3gpp-traffic-influence")->scope, "");
    EXPECT_EQ(sbi_core::http2::Authorizer::required_scope(
                  "/3gpp-traffic-influence/v1/af-1/subscriptions"),
              "");
}

} // namespace
