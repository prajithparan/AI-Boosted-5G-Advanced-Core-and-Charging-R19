#include "qos_mon_collect.hpp"

#include <string>

namespace nwdaf {

std::vector<nlohmann::json> qos_mon_event_notifs(const nlohmann::json& notification) {
    std::vector<nlohmann::json> out;
    if (!notification.is_object() || !notification.contains("eventNotifs")) {
        return out;
    }
    const auto& notifs = notification.at("eventNotifs");
    if (!notifs.is_array()) {
        return out;
    }
    for (const auto& n : notifs) {
        if (n.is_object() && n.value("event", std::string{}) == "QOS_MON") {
            out.push_back(n); // verbatim -- snssai (any SST/SD) and delays preserved
        }
    }
    return out;
}

} // namespace nwdaf
