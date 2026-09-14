#include "cdr_event_producer.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <ctime>
#include <librdkafka/rdkafkacpp.h>

namespace chf {

// Delivery reports arrive on the librdkafka poll thread. They are the ONLY place a failed produce
// is visible, so they must be counted and logged here rather than swallowed.
class CdrEventProducer::DeliveryReport : public RdKafka::DeliveryReportCb {
public:
    DeliveryReport(std::atomic<std::uint64_t>& delivered, std::atomic<std::uint64_t>& failed)
        : delivered_(delivered), failed_(failed) {}

    void dr_cb(RdKafka::Message& message) override {
        if (message.err() == RdKafka::ERR_NO_ERROR) {
            delivered_.fetch_add(1);
            return;
        }
        failed_.fetch_add(1);
        const auto* key = message.key();
        spdlog::error("chf: CDR event delivery FAILED for subscriber {} -- {}. The row is still "
                      "in Doris if direct insert is on; if it is not, this CDR is lost.",
                      key != nullptr ? *key : std::string("?"),
                      message.errstr());
    }

private:
    std::atomic<std::uint64_t>& delivered_;
    std::atomic<std::uint64_t>& failed_;
};

namespace {

// A CDR's timestamp on the wire, as the same UTC string the Doris column holds.
std::string utc(std::time_t t) {
    std::tm tm{};
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime_r(&t, &tm));
    return buf;
}

std::string utc_date(std::time_t t) {
    std::tm tm{};
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", gmtime_r(&t, &tm));
    return buf;
}

} // namespace

std::string CdrEventProducer::to_event_json(const CdrRecord& r) {
    nlohmann::json j;
    // Same names, same order, as CdrWriter's INSERT column list -- keep them in step.
    j["recorded_date"] = utc_date(r.invocation_time_stamp);
    j["charging_data_ref"] = r.charging_data_ref;
    j["invocation_sequence_number"] = r.invocation_sequence_number;
    j["service_type"] = r.service_type;
    j["operation"] = r.operation;
    j["subscriber_identifier"] = r.subscriber_identifier;
    j["nf_consumer_node_functionality"] = r.nf_consumer_node_functionality;
    if (r.rating_group) {
        j["rating_group"] = *r.rating_group;
    }
    if (r.granted_total_volume) {
        j["granted_total_volume"] = *r.granted_total_volume;
    }
    if (r.granted_service_specific_units) {
        j["granted_service_specific_units"] = *r.granted_service_specific_units;
    }
    if (r.used_total_volume) {
        j["used_total_volume"] = *r.used_total_volume;
    }
    if (r.reserved_cost) {
        j["reserved_cost"] = *r.reserved_cost;
    }
    if (r.reserved_cost_currency) {
        j["reserved_cost_currency"] = *r.reserved_cost_currency;
    }
    j["invocation_time_stamp"] = utc(r.invocation_time_stamp);
    j["serving_plmn"] = r.serving_plmn;
    j["is_roaming"] = r.is_roaming;
    j["charging_information_type"] = r.charging_information_type;
    j["service_charging_information"] = r.service_charging_information;
    // asn1_cdr is deliberately NOT carried: it is a hex-encoded BER blob derived from the fields
    // above, and a consumer that needs it re-encodes from them. Sending it would roughly double
    // every event for information the event already contains.
    return j.dump();
}

CdrEventProducer::CdrEventProducer(const CdrEventBusOptions& options)
    : topic_(options.topic), flush_timeout_ms_(options.flush_timeout_ms) {
    if (options.brokers.empty()) {
        spdlog::info("chf: CDR event bus disabled (no brokers configured)");
        return;
    }
    std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    std::string err;
    const auto set = [&](const char* k, const std::string& v) {
        if (conf->set(k, v, err) != RdKafka::Conf::CONF_OK) {
            throw std::runtime_error(std::string("librdkafka config ") + k + ": " + err);
        }
    };
    set("bootstrap.servers", options.brokers);
    set("client.id", options.client_id);
    // The two settings that make this a billing-grade sink rather than a fire-and-forget log:
    // every produce waits for all in-sync replicas, and a retry cannot duplicate a record.
    set("acks", "all");
    set("enable.idempotence", "true");
    report_ = std::make_unique<DeliveryReport>(delivered_, failed_);
    if (conf->set("dr_cb", report_.get(), err) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("librdkafka dr_cb: " + err);
    }
    producer_.reset(RdKafka::Producer::create(conf.get(), err));
    if (!producer_) {
        throw std::runtime_error("librdkafka producer: " + err);
    }
    spdlog::info("chf: CDR event bus ENABLED -> {} topic '{}' (acks=all, idempotent)",
                 options.brokers,
                 topic_);
}

CdrEventProducer::~CdrEventProducer() {
    if (producer_) {
        flush(flush_timeout_ms_);
    }
}

void CdrEventProducer::publish(const CdrRecord& record) {
    if (!producer_) {
        return;
    }
    const auto body = to_event_json(record);
    const auto err = producer_->produce(topic_,
                                        RdKafka::Topic::PARTITION_UA,
                                        RdKafka::Producer::RK_MSG_COPY,
                                        const_cast<char*>(body.data()),
                                        body.size(),
                                        record.subscriber_identifier.data(),
                                        record.subscriber_identifier.size(),
                                        0,
                                        nullptr);
    if (err != RdKafka::ERR_NO_ERROR) {
        failed_.fetch_add(1);
        spdlog::error("chf: CDR event enqueue failed for ChargingDataRef={}: {}",
                      record.charging_data_ref,
                      RdKafka::err2str(err));
    }
    // Serve delivery reports without blocking; the charging request must not wait on the broker.
    producer_->poll(0);
}

void CdrEventProducer::flush(int timeout_ms) {
    if (!producer_) {
        return;
    }
    const auto err = producer_->flush(timeout_ms);
    if (err != RdKafka::ERR_NO_ERROR) {
        spdlog::warn("chf: CDR event bus flush incomplete after {} ms: {} ({} still queued)",
                     timeout_ms,
                     RdKafka::err2str(err),
                     producer_->outq_len());
    } else {
        spdlog::info("chf: CDR event bus flushed -- {} delivered, {} failed",
                     delivered_.load(),
                     failed_.load());
    }
}

} // namespace chf
