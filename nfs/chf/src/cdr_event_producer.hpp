#pragma once

// ADR-0355: the event bus. CHF publishes every CDR as an event to Apache Kafka.
//
// CLAUDE.md's data plane is "NFs emit events -> Kafka -> feature store -> training -> ONNX ->
// in-process inference". Until now only the batch half existed: CDRs went straight into Doris by
// INSERT and a SQL job (ADR-0350) computed features from the table. Nothing was ever *emitted*.
// This is the emitting half.
//
// Why Kafka and not Redpanda, although CLAUDE.md names both: Redpanda's own repository licenses
// its core under the Business Source License and the Redpanda Community License (its licenses/
// directory holds bsl.md and rcl.md). Neither is OSI-approved, and P1 is strict. Apache Kafka is
// Apache-2.0; librdkafka, the client used here, is BSD-2-Clause. Verified from the projects' own
// license files, not recalled.
//
// What this buys beyond "a queue exists": durability CHF never had. ADR-0338's batching disclosed
// that a buffered CDR is not durable and dies with the process. A record produced here with
// acks=all is on the broker's disk -- replicated, in a real deployment -- before the delivery
// report arrives, and enable.idempotence means a retry after a transient failure cannot produce
// it twice. That is the property billing data needs and an in-process buffer cannot give.
//
// DISCLOSED: delivery is asynchronous. publish() enqueues; the broker acknowledges later, and a
// failure surfaces through the delivery-report callback as a logged error and a counter, not as
// an exception at the charging request. That matches CdrWriter::write's own "best effort, never
// block the charging response" discipline. The Nchf response is not held for the broker.
//
// The event body is the CDR's own columns under their own names -- the same nineteen the Doris
// INSERT uses -- so the Routine Load that consumes it maps 1:1 and there is no second vocabulary
// to keep in step. Nothing here invents a field.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "cdr.hpp"

namespace RdKafka {
class Producer;
class DeliveryReportCb;
} // namespace RdKafka

namespace chf {

struct CdrEventBusOptions {
    // Empty disables the bus entirely: every existing deployment keeps its exact current
    // behaviour. Set to "host:port[,host:port]" to enable.
    std::string brokers;
    std::string topic = "chf.cdr";
    // The producer's own client id, so a broker operator can see WHICH CHF instance is producing.
    std::string client_id = "chf";
    // How long shutdown waits for the broker to acknowledge what is queued. Config, not a literal.
    int flush_timeout_ms = 0;
};

class CdrEventProducer {
public:
    explicit CdrEventProducer(const CdrEventBusOptions& options);
    ~CdrEventProducer();
    CdrEventProducer(const CdrEventProducer&) = delete;
    CdrEventProducer& operator=(const CdrEventProducer&) = delete;

    bool enabled() const { return producer_ != nullptr; }

    // Enqueue one CDR as a JSON event, keyed on subscriber_identifier so a subscriber's records
    // land on one partition in order -- the same locality Doris's own bucketing by subscriber
    // gives the table. Never throws; a failure is logged and counted.
    void publish(const CdrRecord& record);

    // Block until every enqueued event is acknowledged or `timeout_ms` passes. Called on
    // shutdown, for the same reason CdrWriter flushes on shutdown (ADR-0353): an orderly stop
    // must not lose billing data that was accepted.
    void flush(int timeout_ms);

    std::uint64_t delivered() const { return delivered_.load(); }
    std::uint64_t failed() const { return failed_.load(); }

    // The event as it goes on the wire. Public so a test can assert the shape without a broker.
    static std::string to_event_json(const CdrRecord& record);

private:
    class DeliveryReport;
    std::unique_ptr<DeliveryReport> report_;
    std::unique_ptr<RdKafka::Producer> producer_;
    std::string topic_;
    int flush_timeout_ms_ = 0;
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::uint64_t> failed_{0};
};

} // namespace chf
