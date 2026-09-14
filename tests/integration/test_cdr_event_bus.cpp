// ADR-0355: a CDR written by CdrWriter arrives on the Kafka topic, keyed on the subscriber.
//
// Gated on a broker being reachable at CHF_CDR_EVENT_BUS_BROKERS (the lab compose value if unset)
// -- skipped, not failed, when there is none, same as the Doris-backed tests. A skip is printed,
// so a CI leg without Kafka does not look like a pass.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <librdkafka/rdkafkacpp.h>
#include <memory>
#include <string>

#include "cdr.hpp"

#include <gtest/gtest.h>

namespace {

std::string brokers() {
    if (const char* env = std::getenv("CHF_CDR_EVENT_BUS_BROKERS")) {
        return env;
    }
    return "127.0.0.1:9092";
}

bool broker_reachable(const std::string& b) {
    std::string err;
    std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    conf->set("bootstrap.servers", b, err);
    std::unique_ptr<RdKafka::Producer> p(RdKafka::Producer::create(conf.get(), err));
    if (!p) {
        return false;
    }
    RdKafka::Metadata* md = nullptr;
    const auto rc = p->metadata(true, nullptr, &md, 3000);
    delete md;
    return rc == RdKafka::ERR_NO_ERROR;
}

} // namespace

TEST(CdrEventBus, ACdrWrittenByCdrWriterArrivesOnTheTopicKeyedOnTheSubscriber) {
    const auto b = brokers();
    if (!broker_reachable(b)) {
        GTEST_SKIP() << "no Kafka broker at " << b << " -- event bus test skipped, not passed";
    }
    // A fresh topic per run by default, so runs cannot see each other's events.
    // CHF_CDR_EVENT_BUS_TOPIC overrides it -- that is how the Doris Routine Load half of ADR-0355
    // was proven, by pointing this test at the topic a Routine Load job was consuming and watching
    // the row land.
    std::string topic = "chf.cdr.test." + std::to_string(std::time(nullptr));
    if (const char* env = std::getenv("CHF_CDR_EVENT_BUS_TOPIC")) {
        topic = env;
    }

    // Consumer first, subscribed before anything is produced, so the offset is not a race.
    std::string err;
    std::unique_ptr<RdKafka::Conf> cconf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    cconf->set("bootstrap.servers", b, err);
    cconf->set("group.id", "test-" + topic, err);
    cconf->set("auto.offset.reset", "earliest", err);
    std::unique_ptr<RdKafka::KafkaConsumer> consumer(
        RdKafka::KafkaConsumer::create(cconf.get(), err));
    ASSERT_TRUE(consumer) << err;
    ASSERT_EQ(consumer->subscribe({topic}), RdKafka::ERR_NO_ERROR);

    // Bus-only: no Doris connection at all. This proves the sink is independent of Doris.
    chf::DorisOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 1; // nothing listens; CdrWriter must degrade, not fail
    opts.user = "root";
    opts.database = "none";
    opts.event_bus_brokers = b;
    opts.event_bus_topic = topic;
    opts.direct_insert = false;
    // The destructor's flush waits this long for the broker's delivery report. Left at the
    // struct default (0 ms) the CDR is still queued when the producer is torn down and is
    // dropped -- "flush incomplete after 0 ms ... (1 still queued)", the exact failure CI's
    // first run of this test showed. config/chf.json sets 10000 for the real CHF.
    opts.event_bus_flush_timeout_ms = 10000;
    chf::CdrRecord r;
    r.charging_data_ref = "chg-bus-1";
    r.invocation_sequence_number = 1;
    r.service_type = "ConvergedCharging";
    r.operation = "Create";
    r.subscriber_identifier = "imsi-999700000000777";
    r.nf_consumer_node_functionality = "SMF";
    r.rating_group = 3;
    r.invocation_time_stamp = std::time(nullptr);
    r.charging_information_type = "PDUSession";
    {
        chf::CdrWriter writer(opts);
        writer.write(r);
        // destructor flushes -- the property ADR-0353 established for the direct path.
    }

    // Now read it back.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool found = false;
    while (std::chrono::steady_clock::now() < deadline && !found) {
        std::unique_ptr<RdKafka::Message> msg(consumer->consume(1000));
        if (msg->err() != RdKafka::ERR_NO_ERROR) {
            continue;
        }
        ASSERT_NE(msg->key(), nullptr);
        EXPECT_EQ(*msg->key(), "imsi-999700000000777") << "event must be keyed on the subscriber";
        const auto j = nlohmann::json::parse(
            std::string(static_cast<const char*>(msg->payload()), msg->len()));
        EXPECT_EQ(j["charging_data_ref"], "chg-bus-1");
        EXPECT_EQ(j["rating_group"], 3);
        EXPECT_EQ(j["charging_information_type"], "PDUSession");
        found = true;
    }
    consumer->close();
    EXPECT_TRUE(found) << "the CDR event never arrived on " << topic;
}
