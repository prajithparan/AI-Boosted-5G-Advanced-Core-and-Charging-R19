// ADR-0311: rated usage -> CDR -> TMF678 line items -> CustomerBill, against a real Doris.
//
// ADR-0310 built the bill run and said plainly that nothing produced its input. This is the test
// for the piece that closes that: CdrWriter could only INSERT before, so no bill could ever be
// generated from real usage. Written against a real Doris rather than a stub because the two
// things most likely to be wrong -- the SQL and the Release-only filter -- are invisible to a mock.

#include <cstdlib>
#include <ctime>
#include <string>

#include "../../bss/balance-management/src/bill_run.hpp"
#include "billing_items.hpp"
#include "cdr.hpp"

#include <gtest/gtest.h>

namespace {

chf::DorisOptions doris_options_from_env() {
    chf::DorisOptions options;
    const char* host = std::getenv("CHF_DORIS_HOST");
    options.host = host != nullptr ? host : "127.0.0.1";
    const char* port = std::getenv("CHF_DORIS_PORT");
    options.port = port != nullptr ? static_cast<std::uint16_t>(std::atoi(port)) : 9030;
    const char* user = std::getenv("CHF_DORIS_USER");
    options.user = user != nullptr ? user : "root";
    const char* password = std::getenv("CHF_DORIS_PASSWORD");
    options.password = password != nullptr ? password : "";
    const char* database = std::getenv("CHF_DORIS_DATABASE");
    options.database = database != nullptr ? database : "chf_cdr";
    return options;
}

// The fixture's event time, and the query window DERIVED from it.
//
// The first version of this test hardcoded a timestamp and a window independently, commented the
// timestamp as a date it was not, and queried a window the row fell outside -- so the test failed
// while the code under test was correct. Deriving the window from the same constant removes that
// entire class of mistake: the two cannot disagree.
constexpr std::time_t kFixtureTimestamp = 1'788'000'000; // 2026-08-29 10:40:00 UTC (verified)

std::string sql_time(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

chf::CdrRecord make_cdr(const std::string& ref,
                        std::int64_t seq,
                        const std::string& operation,
                        const std::string& supi,
                        double cost) {
    chf::CdrRecord record{};
    record.charging_data_ref = ref;
    record.invocation_sequence_number = seq;
    record.service_type = "ConvergedCharging";
    record.operation = operation;
    record.subscriber_identifier = supi;
    record.nf_consumer_node_functionality = "SMF";
    record.rating_group = 10;
    record.used_total_volume = 1'000'000;
    record.reserved_cost = cost;
    record.reserved_cost_currency = "EUR";
    record.invocation_time_stamp = kFixtureTimestamp;
    return record;
}

} // namespace

TEST(CdrBillingChain, RatedUsageBecomesABillThroughRealDoris) {
    chf::CdrWriter writer(doris_options_from_env());
    if (!writer.is_connected()) {
        GTEST_SKIP() << "no Doris reachable -- this test validates the real CDR->bill chain and is "
                        "meaningless against a stub; it runs in CI, which has one";
    }

    const std::string supi = "imsi-99970000000" + std::to_string(::getpid() % 1000);
    const std::string ref = "billing-chain-" + std::to_string(::getpid());

    // A realistic session: Create and Update carry reserved cost too, and Release closes it.
    writer.write(make_cdr(ref, 1, "Create", supi, 2.0));
    writer.write(make_cdr(ref, 2, "Update", supi, 3.0));
    writer.write(make_cdr(ref, 3, "Release", supi, 5.0));

    chf::CdrWriter::CdrQuery query;
    query.period_start = sql_time(kFixtureTimestamp - 3600);
    query.period_end = sql_time(kFixtureTimestamp + 3600);
    query.subscriber_identifier = supi;
    const auto cdrs = writer.query(query);

    // THE assertion that separates a correct invoice from a multiplied one: only the Release row
    // is billable. Billing Create+Update+Release would charge this session 10.0 instead of 5.0.
    ASSERT_EQ(cdrs.size(), 1u) << "the query must return only the session-closing Release row";
    EXPECT_EQ(cdrs[0].operation, "Release");
    EXPECT_EQ(cdrs[0].subscriber_identifier, supi);

    const auto items = chf::cdrs_to_billing_items(cdrs);
    ASSERT_EQ(items.size(), 1u);
    ASSERT_TRUE(items[0].taxExcludedAmount.has_value());
    EXPECT_DOUBLE_EQ(items[0].taxExcludedAmount->value.value_or(0.0), 5.0);
    EXPECT_EQ(items[0].taxExcludedAmount->unit.value_or(""), "EUR");
    EXPECT_FALSE(items[0].isBilled.value_or(true)) << "a fresh line item is not yet billed";

    // Traceability: the charge must lead back to the session that produced it.
    bool has_ref = false;
    for (const auto& c : items[0].characteristic) {
        if (c.name.value_or("") == "chargingDataRef" && c.value == ref) {
            has_ref = true;
        }
    }
    EXPECT_TRUE(has_ref) << "a disputed charge could not be traced to its session";

    const auto bill = billing::run_bill("acct-" + supi,
                                        "BILL-CHAIN-1",
                                        "2026-09-06T00:00:00Z",
                                        query.period_start,
                                        query.period_end,
                                        "2026-09-30T00:00:00Z",
                                        items);
    ASSERT_TRUE(bill.bill.amountDue.has_value());
    EXPECT_DOUBLE_EQ(bill.bill.amountDue->value.value_or(0.0), 5.0)
        << "the bill total must equal the usage actually rated for this session";
    ASSERT_EQ(bill.billed_items.size(), 1u);
    EXPECT_TRUE(bill.billed_items[0].isBilled.value_or(false));
}

TEST(CdrBillingChain, ARowWithNoRatedCostProducesNoLineItem) {
    chf::CdrWriter writer(doris_options_from_env());
    if (!writer.is_connected()) {
        GTEST_SKIP() << "no Doris reachable";
    }

    const std::string supi = "imsi-88870000000" + std::to_string(::getpid() % 1000);
    const std::string ref = "billing-unrated-" + std::to_string(::getpid());
    auto unrated = make_cdr(ref, 1, "Release", supi, 0.0);
    unrated.reserved_cost = std::nullopt; // nothing was committed against the balance
    writer.write(unrated);

    chf::CdrWriter::CdrQuery query;
    query.period_start = sql_time(kFixtureTimestamp - 3600);
    query.period_end = sql_time(kFixtureTimestamp + 3600);
    query.subscriber_identifier = supi;
    const auto cdrs = writer.query(query);
    ASSERT_EQ(cdrs.size(), 1u);

    // Billing an unrated row would charge for usage that was never priced.
    EXPECT_TRUE(chf::cdrs_to_billing_items(cdrs).empty());
}
