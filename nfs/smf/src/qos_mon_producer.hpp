#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

// The SMF's Nsmf_EventExposure QOS_MON producer (ADR-0379). Nsmf_EventExposure had subscription
// CRUD but never delivered notifications (ADR-0201 disclosed exactly this). This turns the SMF into
// a real producer of the SmfEvent QOS_MON event (TS 29.508 EventNotification: snssai + ulDelays/
// dlDelays/rtDelays), which the NWDAF collects and aggregates into SERVICE_EXPERIENCE analytics
// (TS 23.288 6.4).
//
// The per-flow latency is synthesized -- this project has no real UPF QoS-monitoring measurement --
// which is a disclosed lab data source; the subscription matching, the notification shape, and the
// delivery are real. Every S-NSSAI is handled generically: a standardised SST (1/2/3) or an
// operator-specific one (128-255), with or without an SD, is matched verbatim and echoed verbatim
// -- never restricted to an allow-list (a real deployment carries operator-defined slices).

namespace smf::qos_mon {

// One built notification and where it goes (a subscription's notifUri).
struct Notification {
    std::string notif_uri;
    nlohmann::json body; // NsmfEventExposureNotification { notifId, eventNotifs[] }
};

// For every subscription that subscribes to QOS_MON, build one NsmfEventExposureNotification whose
// eventNotifs carry a QOS_MON EventNotification per live SM context matching that subscription's
// optional per-event S-NSSAI filter (absent filter = every session). `tick` seeds the synthesized
// delays so the output is deterministic for a given tick yet varies over time. A subscription with
// no matching session yields no notification (eventNotifs has minItems 1).
std::vector<Notification>
build_qos_mon_notifications(const std::vector<nlohmann::json>& subscriptions,
                            const std::vector<nlohmann::json>& sm_contexts,
                            std::uint64_t tick,
                            const std::string& timestamp);

} // namespace smf::qos_mon
