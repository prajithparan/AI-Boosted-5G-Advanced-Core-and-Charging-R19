#pragma once

#include <nlohmann/json.hpp>

#include <vector>

// Collection side of the SMF QOS_MON feed (ADR-0379 slice 2). The NWDAF subscribes to the SMF's
// Nsmf_EventExposure QOS_MON event and receives NsmfEventExposureNotifications; this extracts the
// per-event QOS_MON reports to append to the collection timeline, which the SERVICE_EXPERIENCE
// analytic (slice 3) aggregates per S-NSSAI.

namespace nwdaf {

// The QOS_MON EventNotifications carried by one NsmfEventExposureNotification (TS 29.508): every
// entry of body.eventNotifs whose `event` is "QOS_MON". Each returned object is one EventNotification
// (its snssai, ul/dl/rtDelays and supi preserved verbatim -- any S-NSSAI, standard or custom).
// Tolerant of a malformed or non-QOS_MON body: returns an empty vector rather than throwing.
std::vector<nlohmann::json> qos_mon_event_notifs(const nlohmann::json& notification);

} // namespace nwdaf
