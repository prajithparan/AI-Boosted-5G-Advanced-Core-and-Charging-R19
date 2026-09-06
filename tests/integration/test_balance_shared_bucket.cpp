// ADR-0307 (C1 of ADR-0300): shared / family / group buckets.
//
// README listed this as the one commercial product outright **Not supported**: balance buckets
// were keyed by SUPI (`bucket.id = supi`), so there was no way for several subscribers to draw
// down one allowance. The fix needed no new resource and no schema change, because TMF654's own
// `Bucket` already carries `isShared` and `relatedParty` and both columns already existed -- which
// is the test's first job to demonstrate, since "we invented a group-bucket concept" and "we used
// the one the standard already has" are very different claims.
//
// Real PostgreSQL, same discipline as this project's other store tests (ADR-0054): skipped with an
// explicit message when no database is reachable, run for real in CI.

#include <cstdlib>
#include <string>

// Included by explicit relative path, NOT via an include directory: bss/product-catalog,
// bss/balance-management and several NFs each have their own `store.hpp`, so putting any of their
// source directories on this target's include path silently changes which one every OTHER test in
// this binary resolves. Adding one is exactly what broke test_product_catalog_postgres.cpp here.
#include "../../bss/balance-management/src/store.hpp"

#include <gtest/gtest.h>

namespace {

std::string test_conninfo() {
    if (const char* env = std::getenv("TEST_BALANCE_POSTGRES_URL")) {
        return env;
    }
    return "postgresql://balance_management:balance_management@localhost:5432/balance_management";
}

bool postgres_reachable(const std::string& conninfo) {
    try {
        pqxx::connection conn(conninfo);
        return conn.is_open();
    } catch (const std::exception&) {
        return false;
    }
}

class SharedBucketTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!postgres_reachable(test_conninfo())) {
            GTEST_SKIP() << "No reachable PostgreSQL at " << test_conninfo()
                         << " -- this test validates the real shared-bucket query and is "
                            "meaningless against a stub; it runs in CI, which has one";
        }
        conn_ = std::make_unique<pqxx::connection>(test_conninfo());
        pqxx::work txn(*conn_);
        txn.exec("DELETE FROM bucket WHERE id LIKE 'test-shared-%'");
        txn.commit();
    }

    void insert_bucket(const std::string& id,
                       bool is_shared,
                       const std::string& related_party_json,
                       const std::string& status) {
        pqxx::work txn(*conn_);
        txn.exec("INSERT INTO bucket (id, is_shared, related_party, status, usage_type) "
                 "VALUES ($1, $2, $3::jsonb, $4, 'monetary')",
                 pqxx::params{id, is_shared, related_party_json, status});
        txn.commit();
    }

    std::unique_ptr<pqxx::connection> conn_;
};

} // namespace

TEST_F(SharedBucketTest, AMemberOfAFamilyBucketResolvesToIt) {
    insert_bucket("test-shared-family-1",
                  true,
                  R"([{"id":"imsi-999700000000901"},{"id":"imsi-999700000000902"}])",
                  "active");

    balance_management::BalanceStore store("https://example.com", test_conninfo());

    // Both members must resolve to the SAME bucket -- that single fact is what "shared bundle"
    // means commercially, and it is what was impossible before.
    const auto first = store.find_shared_bucket_for("imsi-999700000000901");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->id.value_or(""), "test-shared-family-1");

    const auto second = store.find_shared_bucket_for("imsi-999700000000902");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->id.value_or(""), "test-shared-family-1")
        << "two members of one family resolved to different buckets -- they are not sharing";
}

TEST_F(SharedBucketTest, ANonMemberDoesNotDrawFromSomeoneElsesFamilyBucket) {
    insert_bucket("test-shared-family-2", true, R"([{"id":"imsi-999700000000901"}])", "active");

    balance_management::BalanceStore store("https://example.com", test_conninfo());
    EXPECT_FALSE(store.find_shared_bucket_for("imsi-999700000000999").has_value())
        << "a subscriber resolved to a family bucket they do not belong to -- this is the "
           "unrecoverable direction of this feature getting it wrong";
}

TEST_F(SharedBucketTest, ANonSharedBucketIsNeverReturnedAsAGroupBucket) {
    // A personal bucket that happens to name its owner as a relatedParty must NOT be treated as a
    // group bucket, or every subscriber would appear to be in a family of one and the fallback
    // path would stop being exercised.
    insert_bucket("test-shared-personal", false, R"([{"id":"imsi-999700000000903"}])", "active");

    balance_management::BalanceStore store("https://example.com", test_conninfo());
    EXPECT_FALSE(store.find_shared_bucket_for("imsi-999700000000903").has_value());
}

TEST_F(SharedBucketTest, AnExpiredSharedBucketIsNotUsed) {
    // Checked in the store rather than at each call site: an expired family allowance silently
    // absorbing usage is a billing error, and pushing the check outward is how one call site
    // eventually forgets it.
    insert_bucket("test-shared-expired", true, R"([{"id":"imsi-999700000000904"}])", "expired");

    balance_management::BalanceStore store("https://example.com", test_conninfo());
    EXPECT_FALSE(store.find_shared_bucket_for("imsi-999700000000904").has_value());
}
