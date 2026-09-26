// ADR-0385: TMF654 balance management on the consolidated charging DB (schema balance_mgmt).
//
//   * Lossless: fully populated TopupBalance / AdjustBalance / ReserveBalance come back exactly
//     through GET -- including confirmationDate and partyAccount, which the per-service store
//     silently dropped -- and the bucket a top-up creates carries its full
//     PartyAccountRef/products.
//   * Semantics unchanged: reserve, reserve beyond funds (failed, balance untouched), release,
//     adjust never below zero, top-up credits an existing bucket, accumulated balance sums.
//   * The bucket shape bss/provisioning writes (no reserved amount supplied) can be reserved
//     against -- 40-balance.sql left it NULL, and NULL + x would have silently corrupted it.
//   * Concurrency: parallel reserves on one bucket never overdraw (one conditional UPDATE each).
//
// Real PostgreSQL (TEST_BALANCE_POSTGRES_URL, the charging DB); skipped when none is reachable.

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <pqxx/pqxx>
#include <string>
#include <thread>
#include <vector>

#include "../../bss/balance-management/src/store.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

std::string conninfo() {
    if (const char* env = std::getenv("TEST_BALANCE_POSTGRES_URL")) {
        return env;
    }
    return "postgresql://postgres@127.0.0.1:5434/charging";
}

bool reachable() {
    try {
        pqxx::connection c(conninfo());
        return c.is_open();
    } catch (const std::exception&) {
        return false;
    }
}

constexpr const char* kBase = "https://test/tmf-api/prepayBalanceManagement/v4";
constexpr const char* kT1 = "2026-02-03T04:05:06.007Z";
constexpr const char* kT2 = "2026-12-31T23:59:59.000Z";

bss_sid::PartyAccountRef account() {
    return {"acct-lossless", "https://x/acct/1", "billing account", "Acct One", "active"};
}

template <typename Event> void populate_common(Event& e, const std::string& bucket) {
    e.confirmationDate = kT1;
    e.description = "fully populated";
    e.requestedDate = kT2;
    e.amount = bss_sid::Quantity{2.5, "USD"};
    e.bucket = bss_sid::BucketRef{bucket, std::nullopt, std::nullopt};
    e.channel = bss_sid::ChannelRef{"ch-1", "https://x/ch/1", "Web"};
    e.logicalResource = {{"imsi-999700000000501", "https://x/lr/1", "SIM1"},
                         {"msisdn-9997000501", "https://x/lr/2", "MSISDN1"}};
    e.partyAccount = account();
    e.product = {{"prod-1", "https://x/p/1", "P1"}, {"prod-2", "https://x/p/2", "P2"}};
    e.relatedParty = {{"party-1", "https://x/rp/1", "Owner", "owner"},
                      {"party-2", "https://x/rp/2", "Payer", "payer"}};
    e.requestor = bss_sid::RelatedParty{"agent-7", "https://x/agent/7", "Agent", "requestor"};
    e.usageType = "monetary";
    e.validFor = bss_sid::TimePeriod{kT1, kT2};
}

class BalanceLossless : public ::testing::Test {
protected:
    void SetUp() override {
        if (!reachable()) {
            GTEST_SKIP() << "No reachable charging DB at " << conninfo();
        }
        pqxx::connection c(conninfo());
        pqxx::work t(c);
        t.exec("DELETE FROM balance_mgmt.bucket WHERE id LIKE 'test-bal-%'");
        // ADR-0386: buckets reference subscriber_mgmt.account -- the accounts these tests fund.
        t.exec("INSERT INTO subscriber_mgmt.account (id, account_kind) VALUES "
               "('acct-lossless','CONSUMER'), ('acct-sem','CONSUMER') ON CONFLICT DO NOTHING");
        t.commit();
    }
    void provisioned_bucket(const std::string& id, double remaining) {
        // Exactly the columns bss/provisioning writes -- no reserved amount.
        pqxx::connection c(conninfo());
        pqxx::work t(c);
        t.exec("INSERT INTO balance_mgmt.bucket (id, usage_type, remaining_value_unit, "
               "remaining_value_amount, status) VALUES ($1,'monetary','USD',$2,'active')",
               pqxx::params{id, remaining});
        t.commit();
    }
    balance_management::BalanceStore store{kBase, conninfo(), 8};
};

} // namespace

TEST_F(BalanceLossless, FullyPopulatedBalanceEventsRoundTripAndTopupCreatesAFullBucket) {
    bss_sid::TopupBalance topup;
    populate_common(topup, "test-bal-full");
    topup.isAutoTopup = true;
    topup.numberOfPeriods = 3;
    topup.reason = "promo";
    topup.voucher = "V-123";
    topup.amount = bss_sid::Quantity{10.123456789, "USD"}; // more than 6 decimals, exact
    topup.balanceTopup = bss_sid::RelatedTopupBalance{"tb-0", "https://x/tb/0", "Parent", "parent"};
    topup.paymentMethod = bss_sid::PaymentMethodRef{"pm-1", "https://x/pm/1", "Card"};
    topup.recurringPeriod = "monthly";
    const auto t = store.topup(topup);
    ASSERT_TRUE(t.succeeded);
    EXPECT_EQ(json(*store.get_topup(*t.record.id)), json(t.record));

    const auto bucket = store.get_bucket("test-bal-full");
    ASSERT_TRUE(bucket.has_value());
    EXPECT_EQ(json(*bucket->partyAccount), json(account())) << "PartyAccountRef must be whole";
    EXPECT_EQ(json(bucket->product), json(topup.product));
    EXPECT_DOUBLE_EQ(*bucket->remainingValue->value, 10.123456789);
    EXPECT_DOUBLE_EQ(*bucket->reservedValue->value, 0.0);

    bss_sid::AdjustBalance adjust;
    populate_common(adjust, "test-bal-full");
    adjust.reason = "goodwill";
    adjust.adjustType = "oneTime";
    const auto a = store.adjust(adjust);
    ASSERT_TRUE(a.succeeded);
    EXPECT_EQ(json(*store.get_adjust(*a.record.id)), json(a.record));

    bss_sid::ReserveBalance reserve;
    populate_common(reserve, "test-bal-full");
    reserve.reason = "session";
    const auto r = store.reserve(reserve);
    ASSERT_TRUE(r.succeeded);
    EXPECT_EQ(json(*store.get_reserve(*r.record.id)), json(r.record));
    EXPECT_EQ(store.get_reserve(*r.record.id)->confirmationDate, kT1);
}

TEST_F(BalanceLossless, BalanceSemanticsAreUnchanged) {
    bss_sid::TopupBalance fund;
    fund.bucket = bss_sid::BucketRef{"test-bal-sem", std::nullopt, std::nullopt};
    fund.amount = bss_sid::Quantity{10.0, "USD"};
    fund.partyAccount = bss_sid::PartyAccountRef{"acct-sem"};
    ASSERT_TRUE(store.topup(fund).succeeded);
    ASSERT_TRUE(store.topup(fund).succeeded); // credits the existing bucket: 20
    EXPECT_DOUBLE_EQ(*store.get_bucket("test-bal-sem")->remainingValue->value, 20.0);

    const auto reserve = [&](double amount) {
        bss_sid::ReserveBalance r;
        r.bucket = bss_sid::BucketRef{"test-bal-sem", std::nullopt, std::nullopt};
        r.amount = bss_sid::Quantity{amount, "USD"};
        return store.reserve(r);
    };
    EXPECT_TRUE(reserve(15.0).succeeded);
    const auto over = reserve(6.0); // 5 remain
    EXPECT_FALSE(over.succeeded);
    EXPECT_EQ(over.record.status, "failed");
    EXPECT_TRUE(reserve(-15.0).succeeded); // release
    const auto b = store.get_bucket("test-bal-sem");
    EXPECT_DOUBLE_EQ(*b->remainingValue->value, 20.0);
    EXPECT_DOUBLE_EQ(*b->reservedValue->value, 0.0);

    bss_sid::AdjustBalance below;
    below.bucket = bss_sid::BucketRef{"test-bal-sem", std::nullopt, std::nullopt};
    below.amount = bss_sid::Quantity{-21.0, "USD"};
    EXPECT_FALSE(store.adjust(below).succeeded) << "adjust must never take a bucket below zero";

    EXPECT_DOUBLE_EQ(*store.get_accumulated_balance("acct-sem").totalBalance->value, 20.0);
}

TEST_F(BalanceLossless, AProvisionedBucketCanBeReservedAgainst) {
    provisioned_bucket("test-bal-prov", 5.0);
    bss_sid::ReserveBalance r;
    r.bucket = bss_sid::BucketRef{"test-bal-prov", std::nullopt, std::nullopt};
    r.amount = bss_sid::Quantity{2.0, "USD"};
    ASSERT_TRUE(store.reserve(r).succeeded);
    const auto b = store.get_bucket("test-bal-prov");
    EXPECT_DOUBLE_EQ(*b->remainingValue->value, 3.0);
    EXPECT_DOUBLE_EQ(*b->reservedValue->value, 2.0) << "reserved must not be NULL-poisoned";
}

TEST_F(BalanceLossless, ParallelReservesNeverOverdraw) {
    provisioned_bucket("test-bal-race", 100.0);
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < 2; ++j) {
                bss_sid::ReserveBalance r;
                r.bucket = bss_sid::BucketRef{"test-bal-race", std::nullopt, std::nullopt};
                r.amount = bss_sid::Quantity{5.0, "USD"};
                if (store.reserve(r).succeeded) {
                    ++ok;
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(ok.load(), 20) << "32 reserves of 5 against 100: exactly 20 may succeed";
    const auto b = store.get_bucket("test-bal-race");
    EXPECT_DOUBLE_EQ(*b->remainingValue->value, 0.0);
    EXPECT_DOUBLE_EQ(*b->reservedValue->value, 100.0);
}

TEST_F(BalanceLossless, ToppingUpForAnUnknownAccountIsA400) {
    bss_sid::TopupBalance t;
    t.bucket = bss_sid::BucketRef{"test-bal-noacct", std::nullopt, std::nullopt};
    t.amount = bss_sid::Quantity{1.0, "USD"};
    t.partyAccount = bss_sid::PartyAccountRef{"acct-does-not-exist"};
    EXPECT_THROW(store.topup(t), balance_management::InvalidRequest);
    EXPECT_FALSE(store.get_bucket("test-bal-noacct").has_value());
}

TEST_F(BalanceLossless, MalformedDateTimeIsRejectedWithoutMovingBalance) {
    provisioned_bucket("test-bal-date", 5.0);
    bss_sid::ReserveBalance r;
    r.bucket = bss_sid::BucketRef{"test-bal-date", std::nullopt, std::nullopt};
    r.amount = bss_sid::Quantity{1.0, "USD"};
    r.requestedDate = "not-a-date";
    EXPECT_THROW(store.reserve(r), balance_management::InvalidRequest);
    EXPECT_DOUBLE_EQ(*store.get_bucket("test-bal-date")->remainingValue->value, 5.0)
        << "a rejected request must roll back the balance movement";
}
