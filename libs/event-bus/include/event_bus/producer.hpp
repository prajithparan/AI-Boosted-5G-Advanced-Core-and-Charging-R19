#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// Apache Kafka producer for NF events (ADR-0365). acks=all and enable.idempotence, the two
// settings that make a produced record durable on the broker's disk before the delivery report,
// and a retry unable to duplicate it -- the same two nfs/chf/src/cdr_event_producer.cpp
// (ADR-0355) chose for billing data. publish() enqueues and returns; failures surface through
// the delivery-report callback as a counter and a log line, never as an exception on the
// caller's request path.

namespace RdKafka {
class Producer;
class DeliveryReportCb;
} // namespace RdKafka

namespace event_bus {

struct ProducerOptions {
    std::string brokers;   // "host:port[,host:port]"; empty is a configuration error here
    std::string client_id; // visible to the broker operator: which NF instance produces
    int flush_timeout_ms;  // how long the destructor waits for outstanding delivery reports
};

class Producer {
public:
    explicit Producer(const ProducerOptions& options);
    ~Producer();
    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    // Enqueue one record. The key selects the partition, so every record with the same key is
    // consumed in order by one consumer of a group -- the property the MFAF relies on per
    // mfafCorreId. Returns false when librdkafka refused to enqueue (queue full, unknown topic).
    bool publish(std::string_view topic, std::string_view key, std::string_view value);

    // Block until every enqueued record is acknowledged or the timeout passes. Logged either way.
    void flush(int timeout_ms);

    std::uint64_t delivered() const { return delivered_.load(); }
    std::uint64_t failed() const { return failed_.load(); }

private:
    std::unique_ptr<RdKafka::DeliveryReportCb> report_;
    std::unique_ptr<RdKafka::Producer> producer_;
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::uint64_t> failed_{0};
    int flush_timeout_ms_ = 0;
    std::string client_id_;
};

} // namespace event_bus
