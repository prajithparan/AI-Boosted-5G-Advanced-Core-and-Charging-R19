# The Messaging Framework client (ADR-0365): a thin, NF-neutral librdkafka pair -- an
# acks=all/idempotent Producer and a consumer-group Consumer -- so an NF that needs the bus does
# not carry its own copy of the librdkafka boilerplate. The first consumer is the MFAF, whose
# whole job is to be TS 23.288's "Messaging Framework Adaptor" over this bus (ADR-0359); the CHF's
# CdrEventProducer (ADR-0355) predates this library and is the same code in its own words --
# migrating it here is a follow-up, disclosed in ADR-0365, not a hidden duplicate.
#
# Kafka, not Redpanda, for the licence reason nfs/chf/src/cdr_event_producer.hpp records:
# Apache Kafka is Apache-2.0, librdkafka BSD-2-Clause; Redpanda's core is BSL/RCL, not OSI.

find_package(RdKafka CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(event_bus STATIC
    src/producer.cpp
    src/consumer.cpp
)

target_include_directories(event_bus PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(event_bus
    PUBLIC 5gc_project_options
    PRIVATE RdKafka::rdkafka++ spdlog::spdlog
)

target_compile_features(event_bus PUBLIC cxx_std_23)
