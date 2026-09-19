#include "qos_mon_producer.hpp"

namespace smf::qos_mon {

namespace {

constexpr const char* kQosMon = "QOS_MON";

// Does this subscription subscribe to QOS_MON? If so, return its optional per-event S-NSSAI filter
// (null when the subscription does not scope to a slice).
bool subscribes_qos_mon(const nlohmann::json& subscription, nlohmann::json& slice_filter) {
    if (!subscription.contains("eventSubs") || !subscription["eventSubs"].is_array()) {
        return false;
    }
    for (const auto& event_sub : subscription["eventSubs"]) {
        if (event_sub.is_object() && event_sub.value("event", std::string{}) == kQosMon) {
            if (event_sub.contains("snssai")) {
                slice_filter = event_sub["snssai"]; // matched verbatim below -- any SST/SD
            }
            return true;
        }
    }
    return false;
}

// A deterministic synthesized one-way delay in milliseconds (5..54), from a hash of the session key
// and the tick. Deterministic so tests are stable; varies with tick so a stream of notifications
// carries changing values. Disclosed lab data -- no real UPF QoS measurement exists.
unsigned synth_delay_ms(const std::string& key, std::uint64_t tick, unsigned salt) {
    std::uint64_t h = tick * 2654435761ULL + salt;
    for (const char c : key) {
        h = h * 131ULL + static_cast<unsigned char>(c);
    }
    return 5U + static_cast<unsigned>(h % 50ULL);
}

} // namespace

std::vector<Notification>
build_qos_mon_notifications(const std::vector<nlohmann::json>& subscriptions,
                            const std::vector<nlohmann::json>& sm_contexts,
                            std::uint64_t tick,
                            const std::string& timestamp) {
    std::vector<Notification> out;
    for (const auto& subscription : subscriptions) {
        nlohmann::json slice_filter; // null unless the subscription scopes to a slice
        if (!subscribes_qos_mon(subscription, slice_filter)) {
            continue;
        }
        const std::string notif_uri = subscription.value("notifUri", std::string{});
        if (notif_uri.empty()) {
            continue;
        }

        nlohmann::json event_notifs = nlohmann::json::array();
        for (const auto& context : sm_contexts) {
            if (!context.contains("sNssai")) {
                continue; // a session with no slice cannot be reported per-slice
            }
            const nlohmann::json& snssai = context["sNssai"];
            // Exact, verbatim match on the full S-NSSAI (SST plus optional SD). Works for a
            // standardised SST (1/2/3) and an operator-specific one (128-255) alike; an absent
            // filter matches every session.
            if (!slice_filter.is_null() && snssai != slice_filter) {
                continue;
            }
            const std::string supi = context.value("supi", std::string{});
            const std::string key = supi + snssai.dump();
            const unsigned ul = synth_delay_ms(key, tick, 1);
            const unsigned dl = synth_delay_ms(key, tick, 2);

            nlohmann::json notification;
            notification["event"] = kQosMon;
            if (!supi.empty()) {
                notification["supi"] = supi;
            }
            notification["snssai"] = snssai; // echoed verbatim -- never coerced to a known slice
            notification["timeStamp"] = timestamp;
            notification["ulDelays"] = nlohmann::json::array({ul});
            notification["dlDelays"] = nlohmann::json::array({dl});
            notification["rtDelays"] = nlohmann::json::array({ul + dl});
            event_notifs.push_back(std::move(notification));
        }
        if (event_notifs.empty()) {
            continue; // EventNotification list has minItems 1 -- nothing to send
        }

        nlohmann::json body;
        body["notifId"] = subscription.value("notifId", std::string{});
        body["eventNotifs"] = std::move(event_notifs);
        out.push_back(Notification{notif_uri, std::move(body)});
    }
    return out;
}

} // namespace smf::qos_mon
