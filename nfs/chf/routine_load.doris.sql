-- ADR-0355: Doris consumes CHF's CDR events from Kafka itself.
--
-- With this job running, CHF can be configured cdr_direct_insert=false and the table is fed
-- entirely through the event bus: CHF -> Kafka (acks=all) -> Doris Routine Load -> cdr. The
-- process that charges never holds a Doris connection, and a Doris outage no longer stalls or
-- loses CDRs -- they wait on the broker until Doris is back.
--
-- Column names are the event's JSON keys, which are the table's own column names, so there is
-- no mapping to keep in step. `asn1_cdr` is not carried on the event (see cdr_event_producer.hpp)
-- and is left NULL here; a consumer needing the BER encoding re-encodes from the columns.
--
-- The table is UNIQUE KEY on (recorded_date, subscriber_identifier, charging_data_ref,
-- invocation_sequence_number, service_type), so a redelivered event is a no-op merge, not a
-- duplicate row. Exactly-once from the producer (enable.idempotence) plus a key-merging table is
-- what makes the whole path safe to retry.
--
-- Run once against the database that owns the cdr table, e.g.
--   mysql -h127.0.0.1 -P9030 -uroot -D chf_prod < nfs/chf/routine_load.doris.sql
-- Brokers/topic here are the lab compose values (Doris reaches the broker on the compose network's
-- INTERNAL listener, kafka:29092); a deployment substitutes its own.

CREATE ROUTINE LOAD cdr_from_event_bus ON cdr
COLUMNS(recorded_date, charging_data_ref, invocation_sequence_number, service_type, operation,
        subscriber_identifier, nf_consumer_node_functionality, rating_group,
        granted_total_volume, granted_service_specific_units, used_total_volume,
        reserved_cost, reserved_cost_currency, invocation_time_stamp, serving_plmn,
        is_roaming, charging_information_type, service_charging_information)
PROPERTIES (
    "format" = "json",
    "strip_outer_array" = "false",
    "max_batch_interval" = "5",
    "max_batch_rows" = "200000",
    "max_batch_size" = "104857600",
    "max_error_number" = "0"
)
FROM KAFKA (
    "kafka_broker_list" = "kafka:29092",
    "kafka_topic" = "chf.cdr",
    "property.group.id" = "doris-cdr",
    "property.kafka_default_offsets" = "OFFSET_BEGINNING"
);
