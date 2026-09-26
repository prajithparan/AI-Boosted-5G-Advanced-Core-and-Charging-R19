// Real PostgreSQL integration test for bss/roaming-interconnect's InterconnectAgreementStore
// (P4.7). Same not-mocked discipline as test_product_catalog_postgres.cpp. RoamingCdrFileStore is
// NOT tested here -- it has no HTTP route yet (real, disclosed P4.11 scope, still blocked on real
// GSMA TAP3/RAP/NRTRDE spec text, see bss/roaming-interconnect/src/main.cpp's own header).
//
// Requires a real, reachable PostgreSQL with bss/roaming-interconnect/schema.sql already applied.
// SetUp() calls GTEST_SKIP() with an explicit message if unreachable. Override the connection
// target via TEST_ROAMING_INTERCONNECT_POSTGRES_URL.

#include <cstdlib>

// Explicit relative path, NOT a bare "store.hpp" -- integration_tests links against three bss/*
// store libraries that each name their own header src/store.hpp identically; a bare quote-include
// resolves to whichever -I directory CMake happened to list first (a real bug hit and fixed this
// turn: this file's own bare #include "store.hpp" silently resolved to
// bss/product-catalog/src/store.hpp instead, since that library's target_include_directories
// happens to be listed earlier in integration_tests' link order).
#include "../../bss/roaming-interconnect/src/store.hpp"

#include <gtest/gtest.h>

namespace {

std::string test_conninfo() {
    if (const char* env = std::getenv("TEST_ROAMING_INTERCONNECT_POSTGRES_URL")) {
        return env;
    }
    return "postgresql://postgres@127.0.0.1:5434/charging";
}

bool postgres_reachable(const std::string& conninfo) {
    try {
        pqxx::connection conn(conninfo);
        return conn.is_open();
    } catch (const std::exception&) {
        return false;
    }
}

class RoamingInterconnectPostgresTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!postgres_reachable(test_conninfo())) {
            GTEST_SKIP() << "No reachable PostgreSQL at " << test_conninfo()
                         << " (set TEST_ROAMING_INTERCONNECT_POSTGRES_URL, or run CI which "
                            "provisions a real postgres service).";
        }
    }
};

} // namespace

TEST_F(RoamingInterconnectPostgresTest, InterconnectAgreementSurvivesRoundTripThroughRealPostgres) {
    roaming_interconnect::InterconnectAgreementStore store(
        "https://test/tmf-api/agreementManagement/v4/agreement", test_conninfo());

    roaming_interconnect::InterconnectAgreement agreement{};
    agreement.partnerOperatorPlmnId = "31026";
    agreement.agreement.name = "Integration Test Roaming Deal";
    agreement.agreement.agreementType = "RoamingInterconnect";
    agreement.agreement.status = "active";
    agreement.rateTerms = {{"perMbUsd", 0.02}};

    const auto id = store.create(agreement);
    ASSERT_FALSE(id.empty());

    const auto fetched = store.get(id);
    ASSERT_TRUE(fetched.has_value());
    ASSERT_TRUE(fetched->partnerOperatorPlmnId.has_value());
    EXPECT_EQ(*fetched->partnerOperatorPlmnId, "31026");
    EXPECT_EQ(fetched->agreement.name, "Integration Test Roaming Deal");
    EXPECT_EQ(fetched->rateTerms.at("perMbUsd").get<double>(), 0.02);

    // Real cross-process re-derivation, same discipline as every other Postgres-backed store test
    // in this repo.
    roaming_interconnect::InterconnectAgreementStore second_store(
        "https://test/tmf-api/agreementManagement/v4/agreement", test_conninfo());
    const auto refetched = second_store.get(id);
    ASSERT_TRUE(refetched.has_value());
    EXPECT_EQ(refetched->agreement.status, "active");
}

TEST_F(RoamingInterconnectPostgresTest, ListReflectsRealPostgresState) {
    roaming_interconnect::InterconnectAgreementStore store(
        "https://test/tmf-api/agreementManagement/v4/agreement", test_conninfo());

    const auto before = store.list().size();

    roaming_interconnect::InterconnectAgreement agreement{};
    agreement.agreement.name = "Second Integration Test Deal";
    store.create(agreement);

    const auto after = store.list();
    EXPECT_EQ(after.size(), before + 1);
}
