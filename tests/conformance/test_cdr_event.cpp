// ADR-0355: the CDR event as it goes onto the bus. No broker needed -- this is the shape.

#include <nlohmann/json.hpp>

#include "cdr_event_producer.hpp"

#include <gtest/gtest.h>

namespace {

chf::CdrRecord sample() {
    chf::CdrRecord r;
    r.charging_data_ref = "chg-42";
    r.invocation_sequence_number = 3;
    r.service_type = "ConvergedCharging";
    r.operation = "Update";
    r.subscriber_identifier = "imsi-999700000000042";
    r.nf_consumer_node_functionality = "SMF";
    r.rating_group = 7;
    r.used_total_volume = 123456;
    r.reserved_cost = 0.5;
    r.reserved_cost_currency = "EUR";
    r.invocation_time_stamp = 1789300000; // 2026-09-13T11:46:40Z, per `date -u -d @1789300000`
    r.serving_plmn = "999-70";
    r.is_roaming = true;
    r.charging_information_type = "PDUSession";
    r.service_charging_information = R"({"chargingId":1})";
    return r;
}

TEST(CdrEvent, CarriesTheTablesOwnColumnNames) {
    // The Routine Load maps event keys to columns by NAME with no translation layer, so every key
    // here must be a real column of schema.doris.sql's cdr table. A renamed column that is not
    // renamed here would make Doris silently fill NULL for the field -- the same class of loss
    // ADR-0349 found in the direct path.
    const auto j = nlohmann::json::parse(chf::CdrEventProducer::to_event_json(sample()));
    for (const char* col : {"recorded_date",
                            "charging_data_ref",
                            "invocation_sequence_number",
                            "service_type",
                            "operation",
                            "subscriber_identifier",
                            "nf_consumer_node_functionality",
                            "rating_group",
                            "used_total_volume",
                            "reserved_cost",
                            "reserved_cost_currency",
                            "invocation_time_stamp",
                            "serving_plmn",
                            "is_roaming",
                            "charging_information_type",
                            "service_charging_information"}) {
        EXPECT_TRUE(j.contains(col)) << "event is missing column " << col;
    }
    EXPECT_EQ(j["charging_data_ref"], "chg-42");
    EXPECT_EQ(j["rating_group"], 7);
    EXPECT_EQ(j["is_roaming"], true);
    EXPECT_EQ(j["recorded_date"], "2026-09-13");
    EXPECT_EQ(j["invocation_time_stamp"], "2026-09-13 11:46:40");
}

TEST(CdrEvent, AbsentOptionalsAreAbsentNotNullOrZero) {
    // A Release CDR has no rating_group and no used_total_volume. ADR-0350's feature query relies
    // on those being NULL to exclude usage-less rows; an event that sent 0 instead would count a
    // Release as zero usage and dilute every average.
    chf::CdrRecord r = sample();
    r.rating_group.reset();
    r.used_total_volume.reset();
    r.reserved_cost.reset();
    const auto j = nlohmann::json::parse(chf::CdrEventProducer::to_event_json(r));
    EXPECT_FALSE(j.contains("rating_group"));
    EXPECT_FALSE(j.contains("used_total_volume"));
    EXPECT_FALSE(j.contains("reserved_cost"));
}

TEST(CdrEvent, DoesNotCarryTheDerivedAsn1Blob) {
    const auto j = nlohmann::json::parse(chf::CdrEventProducer::to_event_json(sample()));
    EXPECT_FALSE(j.contains("asn1_cdr"));
}

TEST(CdrEvent, BusDisabledWhenNoBrokersConfigured) {
    chf::CdrEventBusOptions opts; // brokers empty
    chf::CdrEventProducer producer(opts);
    EXPECT_FALSE(producer.enabled());
    producer.publish(sample()); // must be a safe no-op, not a crash
    EXPECT_EQ(producer.delivered(), 0u);
    EXPECT_EQ(producer.failed(), 0u);
}

} // namespace
