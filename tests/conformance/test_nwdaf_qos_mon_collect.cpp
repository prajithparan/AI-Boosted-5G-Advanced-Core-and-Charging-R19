// NWDAF collection of the SMF QOS_MON feed (ADR-0379 slice 2): the pure parser that pulls the
// QOS_MON EventNotifications out of an NsmfEventExposureNotification for the collection timeline.
// Verifies any S-NSSAI (standard or custom, with/without SD) is preserved verbatim.

#include <nlohmann/json.hpp>

#include "qos_mon_collect.hpp"

#include <gtest/gtest.h>

namespace {
using nlohmann::json;
}

TEST(NwdafQosMonCollect, ExtractsOnlyQosMonReportsVerbatim) {
    const json notif{
        {"notifId", "n1"},
        {"eventNotifs",
         json::array({json{{"event", "QOS_MON"},
                           {"supi", "imsi-1"},
                           {"snssai", {{"sst", 200}, {"sd", "0a1b2c"}}}, // operator-specific slice
                           {"ulDelays", json::array({5})},
                           {"dlDelays", json::array({7})},
                           {"rtDelays", json::array({12})}},
                      json{{"event", "PDU_SES_REL"}}, // not QOS_MON -> dropped
                      json{{"event", "QOS_MON"}, {"snssai", {{"sst", 1}}}}})}}; // standardised eMBB

    const auto out = nwdaf::qos_mon_event_notifs(notif);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].at("snssai").at("sst"), 200);
    EXPECT_EQ(out[0].at("snssai").at("sd"), "0a1b2c");
    EXPECT_EQ(out[0].at("ulDelays")[0], 5);
    EXPECT_EQ(out[1].at("snssai").at("sst"), 1);
}

TEST(NwdafQosMonCollect, TolerantOfMalformedOrEmpty) {
    EXPECT_TRUE(nwdaf::qos_mon_event_notifs(json::object()).empty());
    EXPECT_TRUE(nwdaf::qos_mon_event_notifs(json::array()).empty());
    EXPECT_TRUE(nwdaf::qos_mon_event_notifs(json{{"eventNotifs", "notanarray"}}).empty());
    EXPECT_TRUE(nwdaf::qos_mon_event_notifs(json{{"eventNotifs", json::array()}}).empty());
}
