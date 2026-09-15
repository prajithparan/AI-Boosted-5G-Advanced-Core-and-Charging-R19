#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Apache Kafka consumer-group consumer (ADR-0365). N replicas of an NF joining the same
// group.id split the topic's partitions between them, so a record is delivered to exactly one
// replica, and a replica that dies hands its partitions to the survivors. That is how the MFAF
// scales delivery without a coordinator and without duplicating notifications per replica.
//
// Offsets are committed AFTER the handler returns (enable.auto.commit=false): a crash mid-handler
// re-delivers the record to whichever replica gets the partition next -- at-least-once, which is
// the right side to err on for data a consumer subscribed to.

namespace RdKafka {
class KafkaConsumer;
} // namespace RdKafka

namespace event_bus {

struct ConsumerOptions {
    std::string brokers;
    std::string group_id;
    std::string client_id;
    std::vector<std::string> topics;
    int poll_timeout_ms; // how long one run() iteration waits for a record before checking stop
};

struct Record {
    std::string topic;
    std::string key;
    std::string value;
    int partition = 0;
    long long offset = 0;
};

class Consumer {
public:
    explicit Consumer(const ConsumerOptions& options);
    ~Consumer();
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    // Poll-handle-commit until stop() is called. Runs on the calling thread.
    void run(const std::function<void(const Record&)>& handler);
    void stop();

private:
    std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
    std::atomic<bool> stop_{false};
    int poll_timeout_ms_ = 1000;
    std::string client_id_;
};

} // namespace event_bus
