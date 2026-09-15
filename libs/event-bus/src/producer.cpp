#include "event_bus/producer.hpp"

#include <spdlog/spdlog.h>

#include <librdkafka/rdkafkacpp.h>
#include <stdexcept>

namespace event_bus {

namespace {

class DeliveryReport : public RdKafka::DeliveryReportCb {
public:
    DeliveryReport(std::atomic<std::uint64_t>& delivered,
                   std::atomic<std::uint64_t>& failed,
                   std::string client_id)
        : delivered_(delivered), failed_(failed), client_id_(std::move(client_id)) {}

    void dr_cb(RdKafka::Message& message) override {
        if (message.err() == RdKafka::ERR_NO_ERROR) {
            delivered_.fetch_add(1);
            return;
        }
        failed_.fetch_add(1);
        spdlog::error("{}: event bus delivery FAILED for key '{}' on {}: {}",
                      client_id_,
                      message.key() ? *message.key() : "",
                      message.topic_name(),
                      message.errstr());
    }

private:
    std::atomic<std::uint64_t>& delivered_;
    std::atomic<std::uint64_t>& failed_;
    std::string client_id_;
};

} // namespace

Producer::Producer(const ProducerOptions& options)
    : flush_timeout_ms_(options.flush_timeout_ms), client_id_(options.client_id) {
    if (options.brokers.empty()) {
        throw std::invalid_argument("event_bus::Producer: no brokers configured");
    }
    std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    std::string err;
    const auto set = [&](const char* key, const std::string& value) {
        if (conf->set(key, value, err) != RdKafka::Conf::CONF_OK) {
            throw std::runtime_error(std::string("librdkafka config ") + key + ": " + err);
        }
    };
    set("bootstrap.servers", options.brokers);
    set("client.id", options.client_id);
    set("acks", "all");
    set("enable.idempotence", "true");
    report_ = std::make_unique<DeliveryReport>(delivered_, failed_, options.client_id);
    if (conf->set("dr_cb", report_.get(), err) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("librdkafka dr_cb: " + err);
    }
    producer_.reset(RdKafka::Producer::create(conf.get(), err));
    if (!producer_) {
        throw std::runtime_error("librdkafka producer: " + err);
    }
    spdlog::info(
        "{}: event bus producer -> {} (acks=all, idempotent)", client_id_, options.brokers);
}

Producer::~Producer() {
    if (producer_) {
        flush(flush_timeout_ms_);
    }
}

bool Producer::publish(std::string_view topic, std::string_view key, std::string_view value) {
    const auto err = producer_->produce(std::string(topic),
                                        RdKafka::Topic::PARTITION_UA,
                                        RdKafka::Producer::RK_MSG_COPY,
                                        const_cast<char*>(value.data()),
                                        value.size(),
                                        key.data(),
                                        key.size(),
                                        0,
                                        nullptr);
    producer_->poll(0);
    if (err != RdKafka::ERR_NO_ERROR) {
        failed_.fetch_add(1);
        spdlog::error("{}: event bus enqueue failed for key '{}': {}",
                      client_id_,
                      key,
                      RdKafka::err2str(err));
        return false;
    }
    return true;
}

void Producer::flush(int timeout_ms) {
    const auto err = producer_->flush(timeout_ms);
    if (err != RdKafka::ERR_NO_ERROR) {
        spdlog::warn("{}: event bus flush incomplete after {} ms: {} ({} still queued)",
                     client_id_,
                     timeout_ms,
                     RdKafka::err2str(err),
                     producer_->outq_len());
    } else {
        spdlog::info("{}: event bus flushed -- {} delivered, {} failed",
                     client_id_,
                     delivered_.load(),
                     failed_.load());
    }
}

} // namespace event_bus
