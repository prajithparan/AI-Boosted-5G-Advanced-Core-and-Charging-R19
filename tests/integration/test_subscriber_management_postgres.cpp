// Real PostgreSQL integration test for bss/subscriber-management's stores (P4.7). Same
// not-mocked discipline as test_product_catalog_postgres.cpp -- talks to an actual libpqxx
// connection against a real postgres instance, exercising the same store.cpp code
// subscriber-management's own binary uses (linked via the subscriber_management_store library).
//
// Requires a real, reachable PostgreSQL with bss/subscriber-management/schema.sql already
// applied. SetUp() attempts a real connection and calls GTEST_SKIP() with an explicit message if
// it fails, rather than either crashing the whole ctest run or pretending to pass. Override the
// connection target via TEST_SUBSCRIBER_MANAGEMENT_POSTGRES_URL.

#include <cstdlib>
#include <ctime>

// Explicit relative path, NOT a bare "store.hpp" -- integration_tests links against three bss/*
// store libraries that each name their own header src/store.hpp identically; a bare quote-include
// resolves to whichever -I directory CMake happened to list first (a real bug hit and fixed this
// turn -- see bss/roaming-interconnect's own test file for the same fix).
#include "../../bss/subscriber-management/src/store.hpp"

#include <gtest/gtest.h>

namespace {

std::string test_conninfo() {
    if (const char* env = std::getenv("TEST_SUBSCRIBER_MANAGEMENT_POSTGRES_URL")) {
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

class SubscriberManagementPostgresTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!postgres_reachable(test_conninfo())) {
            GTEST_SKIP() << "No reachable PostgreSQL at " << test_conninfo()
                         << " (set TEST_SUBSCRIBER_MANAGEMENT_POSTGRES_URL, or run CI which "
                            "provisions a real postgres service). Real DB round-trips in this "
                            "test file are not exercised locally without one.";
        }
    }
};

} // namespace

TEST_F(SubscriberManagementPostgresTest, IndividualSurvivesRoundTripThroughRealPostgres) {
    subscriber_management::PartyIndividualStore store("https://test/tmf-api/party/v4/individual",
                                                      test_conninfo());

    bss_sid::Individual individual{};
    individual.givenName = "Grace";
    individual.familyName = "Hopper";
    individual.fullName = "Grace Hopper";

    const auto id = store.create(individual);
    ASSERT_FALSE(id.empty());

    const auto fetched = store.get(id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->fullName, "Grace Hopper");

    // Real cross-process re-derivation: a second, independent store instance (its own
    // pqxx::connection) must see the same row -- not just an in-process cache.
    subscriber_management::PartyIndividualStore second_store(
        "https://test/tmf-api/party/v4/individual", test_conninfo());
    const auto refetched = second_store.get(id);
    ASSERT_TRUE(refetched.has_value());
    EXPECT_EQ(refetched->familyName, "Hopper");
}

TEST_F(SubscriberManagementPostgresTest, OrganizationListReflectsRealPostgresState) {
    subscriber_management::PartyOrganizationStore store(
        "https://test/tmf-api/party/v4/organization", test_conninfo());

    const auto before = store.list().size();

    bss_sid::Organization organization{};
    organization.name = "Integration Test Org";
    organization.organizationType = "Enterprise";
    store.create(organization);

    const auto after = store.list();
    EXPECT_EQ(after.size(), before + 1);
}

TEST_F(SubscriberManagementPostgresTest, SubscriberIsFindableBySupi) {
    subscriber_management::SubscriberStore store(
        "https://test/bss-api/subscriberManagement/v1/subscriber", test_conninfo());

    // ADR-0386: a subscriber belongs to an account (NOT NULL in subscriber_mgmt), and its SUPI is
    // an active SID Resource -- unique per run so a persistent DB does not 409 on the second run.
    subscriber_management::AccountStore accounts(
        "https://test/bss-api/subscriberManagement/v1/account", test_conninfo());
    subscriber_management::Account account{};
    account.accountKind = "CONSUMER";
    const auto account_id = accounts.create(account);
    const auto supi = "imsi-99970" + std::to_string(std::time(nullptr) % 10000000000LL);

    subscriber_management::Subscriber subscriber{};
    subscriber.supi = supi;
    subscriber.msisdn = "9997" + std::to_string(std::time(nullptr) % 100000000LL);
    subscriber.accountId = account_id;
    subscriber.chargingMode = "PREPAID";
    const auto id = store.create(subscriber);

    const auto by_id = store.get(id);
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->supi, supi);
    EXPECT_EQ(by_id->msisdn, subscriber.msisdn);
    EXPECT_EQ(by_id->accountId, account_id);

    // The same SUPI cannot become a second subscriber's active resource.
    EXPECT_THROW(store.create(subscriber), subscriber_management::Conflict);

    const auto by_supi = store.get_by_supi(supi);
    ASSERT_TRUE(by_supi.has_value());
    EXPECT_EQ(by_supi->id, id);
    EXPECT_EQ(by_supi->chargingMode, "PREPAID");

    EXPECT_FALSE(store.get_by_supi("imsi-000000000000000").has_value());
}

TEST_F(SubscriberManagementPostgresTest, AccountEnterpriseFieldsSurviveRealPostgres) {
    // account.organization_id is a real foreign key into party_organization (schema.sql) -- a
    // real org must exist first, not an arbitrary id.
    subscriber_management::PartyOrganizationStore org_store(
        "https://test/tmf-api/party/v4/organization", test_conninfo());
    bss_sid::Organization organization{};
    organization.name = "Integration Test Enterprise Org";
    const auto org_id = org_store.create(organization);

    subscriber_management::AccountStore store(
        "https://test/bss-api/subscriberManagement/v1/account", test_conninfo());

    subscriber_management::Account account{};
    account.accountKind = "ENTERPRISE";
    account.organizationId = org_id;
    account.billingMode = "SPLIT";
    const auto id = store.create(account);

    const auto fetched = store.get(id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->accountKind, "ENTERPRISE");
    ASSERT_TRUE(fetched->organizationId.has_value());
    EXPECT_EQ(*fetched->organizationId, org_id);
}
