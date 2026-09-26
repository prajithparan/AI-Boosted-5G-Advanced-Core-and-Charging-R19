// Unit tests for oam-gui-bff's pure helpers (no DB, no network).

#include "bff.hpp"
#include "config_mgmt.hpp"
#include "redact.hpp"
#include "util.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>

#ifndef NF_CONFIG_SCHEMA_DIR
#error "NF_CONFIG_SCHEMA_DIR must be defined by CMake"
#endif

namespace {

using nlohmann::json;
using namespace oam_gui_bff;

const std::vector<FieldRule> kOrderRules = {
    {"sim.k", "SECRET_WRITE_ONLY", ""},
    {"sim.opc", "SECRET_WRITE_ONLY", ""},
    {"supi", "PII", "pii:unmask"},
    {"individual.email", "PII", "pii:unmask"},
};

TEST(Redact, StripSecretsRemovesKeysAndKeepsEverythingElse) {
    const json in = {{"supi", "imsi-999700000000001"},
                     {"sim", {{"k", "0123456789ABCDEF0123456789ABCDEF"},
                              {"opc", "FEDCBA9876543210FEDCBA9876543210"},
                              {"sqn", "000000000001"}}}};
    const auto out = strip_secrets(in, kOrderRules);
    EXPECT_EQ(out["sim"]["k"], "[secret: not retained]");
    EXPECT_EQ(out["sim"]["opc"], "[secret: not retained]");
    EXPECT_EQ(out["sim"]["sqn"], "000000000001");
    EXPECT_EQ(out["supi"], "imsi-999700000000001"); // PII is kept for the audit trail
    EXPECT_EQ(out.dump().find("0123456789ABCDEF"), std::string::npos);
}

TEST(Redact, MaskPiiMasksFieldsAndScrubsEmbeddedIdentifiersAndSecrets) {
    const json resp = {{"supi", "imsi-999700000000001"},
                       {"accountId", "acc-999700000000001"},
                       {"individual", {{"email", "a@example.org"}}},
                       {"echo", "k=0123456789abcdef0123456789abcdef"}};
    const auto secrets = secret_values(
        json{{"sim", {{"k", "0123456789ABCDEF0123456789ABCDEF"}}}}, kOrderRules);
    const auto out = mask_pii(resp, kOrderRules, {"imsi-999700000000001", "999700000000001"},
                              secrets);
    EXPECT_EQ(out["supi"], "****************0001");
    EXPECT_EQ(out["accountId"], "acc-***********0001");
    EXPECT_EQ(out["individual"]["email"], "*********.org");
    EXPECT_EQ(out["echo"], "k=[secret: not retained]") << "lower-case echo of an upper-case key";
}

TEST(Util, PkceMatchesRfc7636AppendixB) {
    EXPECT_EQ(pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
              "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST(Util, RandomTokensAreUrlSafeAndDistinct) {
    const auto a = random_token(32), b = random_token(32);
    EXPECT_NE(a, b);
    EXPECT_EQ(a.size(), 43u);
    EXPECT_TRUE(is_safe_id(a));
}

TEST(Util, CookieIsFoundAcrossSplitHeaders) {
    sbi_core::http2::Request req;
    req.headers.emplace("cookie", "a=1; b=2");
    req.headers.emplace("cookie", "__Host-oam_session=tok; c=3");
    EXPECT_EQ(cookie(req, "__Host-oam_session").value_or(""), "tok");
    EXPECT_EQ(cookie(req, "b").value_or(""), "2");
    EXPECT_FALSE(cookie(req, "missing").has_value());
}

TEST(Util, UrlDecodeRoundTripsNonLatinReasons) {
    const std::string reason = "Tarif \xC3\xA9t\xC3\xA9 -- ticket #42";
    EXPECT_EQ(url_decode(url_encode(reason)), reason);
    EXPECT_EQ(url_decode("100%"), "100%");
    EXPECT_EQ(url_decode("%zz"), "%zz");
}

TEST(Util, SafeIds) {
    EXPECT_TRUE(is_safe_id("ord-shop-a.999700000000001"));
    for (const std::string bad : {"", ".", "..", "a/b", "a%2Fb", "a?b", "a b", "a:b"}) {
        EXPECT_FALSE(is_safe_id(bad)) << bad;
    }
}

json load(const std::string& nf) {
    std::ifstream in(std::string(NF_CONFIG_SCHEMA_DIR) + "/" + nf + ".schema.json");
    return json::parse(in);
}

TEST(NfConfig, ValidatorAcceptsTheRealFileAndRejectsDrift) {
    const auto schema = load("product-catalog");
    const json good = {{"port", 7785},
                       {"db_pool_size", 16},
                       {"advertised_ipv4", "127.0.0.1"},
                       {"metrics_bind_address", "0.0.0.0:9473"},
                       {"database_url", "postgresql://postgres@127.0.0.1:5434/charging"}};
    EXPECT_TRUE(NfConfigManager::validate(schema, good).empty());

    json invented = good;
    invented["max_connections"] = 5;
    const auto e1 = NfConfigManager::validate(schema, invented);
    ASSERT_EQ(e1.size(), 1u);
    EXPECT_NE(e1[0].find("max_connections: not a key"), std::string::npos);

    json wrong_type = good;
    wrong_type["port"] = "7785";
    EXPECT_EQ(NfConfigManager::validate(schema, wrong_type).size(), 1u);

    json missing = good;
    missing.erase("database_url");
    const auto e3 = NfConfigManager::validate(schema, missing);
    ASSERT_EQ(e3.size(), 1u);
    EXPECT_EQ(e3[0], "database_url: required");
    // Error text names the path and the rule, never the value (it may be a credential).
    EXPECT_EQ(e1[0].find("postgres"), std::string::npos);
}

TEST(NfConfig, CredentialsAreMaskedAndMaskMeansUnchanged) {
    const auto schema = load("product-catalog");
    const json current = {{"port", 7785}, {"database_url", "postgresql://u:pw@h/db"}};
    const auto shown = NfConfigManager::mask(schema, current);
    EXPECT_EQ(shown["database_url"], kCredentialMask);
    EXPECT_EQ(shown["port"], 7785);

    json proposed = shown;
    proposed["port"] = 7786;
    const auto merged = NfConfigManager::unmask_unchanged(schema, proposed, current);
    EXPECT_EQ(merged["database_url"], "postgresql://u:pw@h/db");
    EXPECT_EQ(merged["port"], 7786);

    proposed["database_url"] = "postgresql://u:new@h/db"; // replaced as a whole
    EXPECT_EQ(NfConfigManager::unmask_unchanged(schema, proposed, current)["database_url"],
              "postgresql://u:new@h/db");
}

} // namespace
