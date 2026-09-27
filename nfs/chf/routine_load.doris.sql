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
--   mysql -h127.0.0.1 -P9030 -uroot -D chf_cdr < nfs/chf/routine_load.doris.sql
-- Brokers/topic here are the lab compose values (Doris reaches the brokers on the compose
-- network's INTERNAL listener; ADR-0443 made this a real 3-broker cluster, kafka-1/2/3:29092,
-- replication.factor=3 -- was a single broker, kafka:29092); a deployment substitutes its own.
-- "kafka_partitions" is deliberately left unset: the real, current CREATE ROUTINE LOAD reference
-- (https://doris.apache.org/docs/dev/sql-manual/sql-statements/data-modification/load-and-export/
-- CREATE-ROUTINE-LOAD/, fetched before sizing chf.cdr's partitions up in ADR-0443, not recalled
-- from memory) states: "If not specified, defaults to subscribing to all partitions under the
-- topic from OFFSET_END" -- so growing the partition count here needs no change to this job. This
-- was NOT independently re-verified against the specific installed image's own local docs/behavior
-- (unlike ADR-0392's pg_partman verification, which did run the real thing); it rests on the
-- public docs for the Doris version family this project uses, disclosed as such.

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
    "kafka_broker_list" = "kafka-1:29092,kafka-2:29092,kafka-3:29092",
    "kafka_topic" = "chf.cdr",
    "property.group.id" = "doris-cdr",
    "property.kafka_default_offsets" = "OFFSET_BEGINNING"
);
