## ADR-0355: the event bus -- CHF emits every CDR to Apache Kafka, and Doris consumes it

**Date:** 2026-09-14. **Status:** Closed (work pushed; last citing commit 49af7b1 on origin/main, 2026-10-06). accepted. Item 4 of the user's pre-NWDAF list.

CLAUDE.md's data plane is *NFs emit events -> Kafka -> feature store -> training -> ONNX ->
in-process inference*. Only the batch half existed: CDRs went straight to Doris by INSERT and a SQL
job computed features from the table (ADR-0350). Nothing was ever emitted. This is the emitting
half, and the consuming half.

### Kafka, not Redpanda -- decided on license, verified at the source

CLAUDE.md names both. Redpanda's own repository licenses its core under the Business Source
License and the Redpanda Community License -- its `licenses/` directory holds `bsl.md` and
`rcl.md`. Neither is OSI-approved, and P1 is strict. Read from the repository, not recalled.
Apache Kafka is Apache-2.0; librdkafka, the C/C++ client, is BSD-2-Clause. The JVM Kafka needs is
the cost of the license; it was also the reason the bus waited until the 3M-CDR soak had released
Doris' memory.

### What was built

- `chf::CdrEventProducer` (librdkafka, `acks=all`, `enable.idempotence=true`) publishes each CDR
  as JSON, keyed on `subscriber_identifier` so one subscriber's records land on one partition in
  order -- the same locality Doris' own bucketing gives the table.
- `CdrWriter` owns the producer and publishes from `write()`, so every CDR call site (main,
  charging engine, Diameter) goes through one place. Three config keys, all defaulting to "nothing
  changes": `cdr_event_bus_brokers` (empty = off), `cdr_event_bus_topic`, `cdr_direct_insert`.
  `nf_config::optional<>` was added so those keys honour the environment the way `require<>` does
  -- `config.value()` ignores it, which is precisely what left the 3M run on a dead Postgres port.
- `nfs/chf/routine_load.doris.sql`: Doris pulls the topic itself. With `cdr_direct_insert=false`
  the charging process holds no Doris connection at all and a Doris outage stalls nothing -- the
  events wait on the broker.
- The event carries the table's own nineteen column names, so the Routine Load maps 1:1 and there
  is no second vocabulary to keep in step. `asn1_cdr` is not carried: it is derived from the other
  fields and would double every event.
- Compose gains a KRaft single-node Kafka with two listeners: host clients on `127.0.0.1:9092`,
  compose-network clients (Doris) on `kafka:29092`. One advertised address cannot serve both.

### What it buys beyond a queue

Durability the direct path never had. ADR-0338's batching disclosed that a buffered CDR is not
durable; ADR-0353 found the flush that was meant to save it was unreachable. A record produced
with `acks=all` is on the broker's disk before the delivery report arrives, and idempotence means a
retry cannot produce it twice. On the consuming side the table's UNIQUE KEY makes a redelivered
event a merge, not a duplicate row. Retry-safe at both ends.

### Verified, not assumed

- Shape: 4 conformance tests, including that a Release's absent `rating_group` and
  `used_total_volume` stay *absent* rather than becoming 0 -- ADR-0350's feature query relies on
  NULL there, and a 0 would count a Release as zero usage and dilute every average.
- CHF -> topic: a CDR written through `CdrWriter` with direct insert **off** and Doris
  deliberately unreachable arrived on the topic keyed on the subscriber. `1 delivered, 0 failed`.
- Topic -> Doris: the Routine Load consumed that event into a scratch `cdr` table, then a full
  Create/Update/Release session pushed through the bus: `loadedRows: 4, errorRows: 0`, Release
  row NULL/NULL, timestamps UTC.

### Disclosed

- Delivery is asynchronous. `publish()` enqueues; a failure surfaces as a logged error and a
  counter on the delivery-report thread, not as an exception to the charging request. Same
  best-effort discipline `CdrWriter::write` has always had.
- The lab broker is PLAINTEXT. CDRs are billing data; a deployment puts TLS and SASL on the
  listener. Not done here, and not claimed.
- The Routine Load's `kafka_broker_list` is the lab compose value. A deployment substitutes its
  own; there is no templating.
- Only CHF emits. CLAUDE.md says "NFs emit events" -- AMF/SMF/UDM emit nothing yet. The producer
  is CHF-private under the no-shared-private-headers rule; lifting it to `libs/` is the step that
  lets other NFs use it, and it is not taken until a second NF needs it.
- CI has no Kafka service. The integration test skips with a printed reason when no broker
  answers, so a CI leg without Kafka does not look like a pass. Adding the service to the workflow
  is a separate change against the free-tier runner budget.

### Rejected

- **Redpanda** -- license, above.
- **A C++ consumer writing the feature store.** Doris already consumes Kafka natively; a second
  consumer would be a second place for the column mapping to drift.
- **Replacing the direct insert outright.** Defaults keep it, so a deployment that has not opted
  in changes nothing, and CI -- which has Doris but no Kafka -- keeps its tests.

