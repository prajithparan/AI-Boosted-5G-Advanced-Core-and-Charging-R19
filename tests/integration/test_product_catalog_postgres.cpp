// Real PostgreSQL integration test for bss/product-catalog's stores (ADR-0054). Deliberately not
// mocked: talks to an actual libpqxx connection against a real postgres instance, exercising the
// same store.cpp code product-catalog's own binary uses (linked via the product_catalog_store
// library -- see bss/product-catalog/CMakeLists.txt).
//
// Requires a real, reachable PostgreSQL with bss/product-catalog/schema.sql already applied.
// CI (.github/workflows/ci.yml) runs a real `postgres` service container and applies the schema
// before `ctest`, so this test exercises the real path there. For a local `ctest` run without a
// Postgres instance running, this is NOT silently skipped without saying so -- SetUp() attempts a
// real connection and calls GTEST_SKIP() with an explicit message if it fails, rather than either
// crashing the whole ctest run or pretending to pass. Override the connection target via
// TEST_POSTGRES_URL (same conninfo shape as product-catalog's own PRODUCT_CATALOG_DATABASE_URL).

#include <cstdlib>

#include "store.hpp"

#include <gtest/gtest.h>

namespace {

std::string test_conninfo() {
    if (const char* env = std::getenv("TEST_POSTGRES_URL")) {
        return env;
    }
    return "postgresql://postgres@127.0.0.1:5434/charging";
}

// Returns true if `conninfo` is actually reachable right now -- used to decide whether to skip,
// so tests don't crash the whole ctest run over an environment precondition CI supplies but a
// bare local checkout may not.
bool postgres_reachable(const std::string& conninfo) {
    try {
        pqxx::connection conn(conninfo);
        return conn.is_open();
    } catch (const std::exception&) {
        return false;
    }
}

class ProductCatalogPostgresTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!postgres_reachable(test_conninfo())) {
            GTEST_SKIP() << "No reachable PostgreSQL at " << test_conninfo()
                         << " (set TEST_POSTGRES_URL, or run CI which provisions a real "
                            "postgres service -- see .github/workflows/ci.yml). Real DB "
                            "round-trips in this test file are not exercised locally without one.";
        }
    }
};

} // namespace

TEST_F(ProductCatalogPostgresTest, ProductOfferingSurvivesRoundTripThroughRealPostgres) {
    product_catalog::ProductOfferingStore store(
        "https://test/tmf-api/productCatalogManagement/v4/productOffering",
        test_conninfo(),
        /*pool_size=*/1);

    bss_sid::ProductOffering offering{};
    offering.name = "Integration Test Offering";
    offering.lifecycleStatus = "Active";
    offering.isSellable = true;
    bss_sid::CategoryRef category{};
    category.id = "cat-test";
    category.name = "Test Category";
    offering.category.push_back(category);

    const auto id = store.create(offering);
    ASSERT_FALSE(id.empty());

    const auto fetched = store.get(id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->name, "Integration Test Offering");
    ASSERT_EQ(fetched->category.size(), 1u);
    EXPECT_EQ(fetched->category[0].id, "cat-test");

    // Real cross-process re-derivation: a second, independent store instance (its own
    // pqxx::connection) must see the same row -- not just an in-process cache.
    product_catalog::ProductOfferingStore second_store(
        "https://test/tmf-api/productCatalogManagement/v4/productOffering",
        test_conninfo(),
        /*pool_size=*/1);
    const auto refetched = second_store.get(id);
    ASSERT_TRUE(refetched.has_value());
    EXPECT_EQ(refetched->name, "Integration Test Offering");

    EXPECT_TRUE(store.remove(id));
    EXPECT_FALSE(store.get(id).has_value());
}

TEST_F(ProductCatalogPostgresTest, ProductSpecificationCharacteristicsSurviveRealPostgres) {
    product_catalog::ProductSpecificationStore store(
        "https://test/tmf-api/productCatalogManagement/v4/productSpecification",
        test_conninfo(),
        /*pool_size=*/1);

    bss_sid::ProductSpecification spec{};
    spec.name = "Integration Test Slice Spec";
    bss_sid::ProductSpecificationCharacteristic characteristic{};
    characteristic.id = "char-test";
    characteristic.name = "Test Characteristic";
    characteristic.configurable = true;
    spec.productSpecCharacteristic.push_back(characteristic);

    const auto id = store.create(spec);
    const auto fetched = store.get(id);
    ASSERT_TRUE(fetched.has_value());
    ASSERT_EQ(fetched->productSpecCharacteristic.size(), 1u);
    EXPECT_EQ(fetched->productSpecCharacteristic[0].name, "Test Characteristic");
    EXPECT_TRUE(*fetched->productSpecCharacteristic[0].configurable);

    EXPECT_TRUE(store.remove(id));
}

TEST_F(ProductCatalogPostgresTest, ListReflectsRealPostgresState) {
    product_catalog::ProductOfferingPriceStore store(
        "https://test/tmf-api/productCatalogManagement/v4/productOfferingPrice",
        test_conninfo(),
        /*pool_size=*/1);

    const auto before = store.list().size();

    bss_sid::ProductOfferingPrice price{};
    price.name = "Integration Test Price";
    price.priceType = "recurring";
    const auto id = store.create(price);

    const auto after = store.list();
    EXPECT_EQ(after.size(), before + 1);

    store.remove(id);
}
