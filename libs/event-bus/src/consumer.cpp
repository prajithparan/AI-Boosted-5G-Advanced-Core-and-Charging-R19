#include "event_bus/consumer.hpp"

#include <spdlog/spdlog.h>

#include <librdkafka/rdkafkacpp.h>
#include <stdexcept>

namespace event_bus {

Consumer::Consumer(const ConsumerOptions& options)
    : poll_timeout_ms_(options.poll_timeout_ms), client_id_(options.client_id) {
    if (options.brokers.empty()) {
        throw std::invalid_argument("event_bus::Consumer: no brokers configured");
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
    set("group.id", options.group_id);
    // Commit only after the handler has run (see the header); start from the beginning for a
    // group that has never committed, so a record produced before the first replica came up is
    // not lost.
    set("enable.auto.commit", "false");
    set("auto.offset.reset", "earliest");
    consumer_.reset(RdKafka::KafkaConsumer::create(conf.get(), err));
    if (!consumer_) {
        throw std::runtime_error("librdkafka consumer: " + err);
    }
    if (const auto rc = consumer_->subscribe(options.topics); rc != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("librdkafka subscribe: " + RdKafka::err2str(rc));
    }
    spdlog::info("{}: event bus consumer group '{}' on {} topic(s) via {}",
                 client_id_,
                 options.group_id,
                 options.topics.size(),
                 options.brokers);
}

Consumer::~Consumer() {
    if (consumer_) {
        consumer_->close();
    }
}

void Consumer::run(const std::function<void(const Record&)>& handler) {
    while (!stop_.load()) {
        std::unique_ptr<RdKafka::Message> message(consumer_->consume(poll_timeout_ms_));
        switch (message->err()) {
            case RdKafka::ERR_NO_ERROR: {
                Record record;
                record.topic = message->topic_name();
                if (message->key()) {
                    record.key = *message->key();
                }
                record.value.assign(static_cast<const char*>(message->payload()), message->len());
                record.partition = message->partition();
                record.offset = message->offset();
                try {
                    handler(record);
                } catch (const std::exception& e) {
                    // The record is still committed: a handler that throws on a record would
                    // throw on it again forever and stall the partition. Logged, counted by the
                    // caller, moved past.
                    spdlog::error("{}: event bus handler threw on {}[{}]@{}: {}",
                                  client_id_,
                                  record.topic,
                                  record.partition,
                                  record.offset,
                                  e.what());
                }
                consumer_->commitAsync(message.get());
                break;
            }
            case RdKafka::ERR__TIMED_OUT:
            case RdKafka::ERR__PARTITION_EOF:
                break;
            default:
                spdlog::warn("{}: event bus consume error: {}", client_id_, message->errstr());
                break;
        }
    }
}

void Consumer::stop() {
    stop_.store(true);
}

} // namespace event_bus
