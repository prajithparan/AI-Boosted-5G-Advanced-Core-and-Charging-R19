// The SMF's Nsmf_EventExposure QOS_MON producer (ADR-0379): the pure notification builder that
// turns subscriptions + live SM contexts into NsmfEventExposureNotifications. Verifies the generic
// S-NSSAI handling the user required -- a standardised SST (1/2/3) and an operator-specific one
// (128-255, with an SD) are both matched and echoed verbatim, never coerced or dropped.

#include "qos_mon_producer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

// A live SM context as the store holds it (the SmContextCreateData subset we read).
json session(const std::string& supi, const json& snssai) {
    return json{{"supi", supi}, {"sNssai", snssai}};
}

json qos_mon_subscription(const std::string& notif_id,
                          const std::string& notif_uri,
                          const json& slice_filter = nullptr) {
    json event_sub{{"event", "QOS_MON"}};
    if (!slice_filter.is_null()) {
        event_sub["snssai"] = slice_filter;
    }
    return json{{"notifId", notif_id}, {"notifUri", notif_uri}, {"eventSubs", json::array({event_sub})}};
}

} // namespace

TEST(SmfQosMon, ReportsEverySliceVerbatimStandardAndCustom) {
    const json embb{{"sst", 1}};                          // standardised eMBB
    const json custom{{"sst", 200}, {"sd", "0a1b2c"}};    // operator-specific slice with an SD
    const std::vector<json> sessions{session("imsi-111", embb), session("imsi-222", custom)};
    const std::vector<json> subs{qos_mon_subscription("n1", "https://nwdaf.example/notify")};

    const auto out = smf::qos_mon::build_qos_mon_notifications(subs, sessions, 7, "2026-09-19T00:00:00Z");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].notif_uri, "https://nwdaf.example/notify");
    EXPECT_EQ(out[0].body.at("notifId"), "n1");
    const auto& notifs = out[0].body.at("eventNotifs");
    ASSERT_EQ(notifs.size(), 2u);

    // Both slices are present, echoed byte-for-byte (custom SST + SD not lost).
    std::vector<json> seen_slices{notifs[0].at("snssai"), notifs[1].at("snssai")};
    EXPECT_NE(std::find(seen_slices.begin(), seen_slices.end(), embb), seen_slices.end());
    EXPECT_NE(std::find(seen_slices.begin(), seen_slices.end(), custom), seen_slices.end());

    for (const auto& n : notifs) {
        EXPECT_EQ(n.at("event"), "QOS_MON");
        EXPECT_TRUE(n.contains("supi"));
        ASSERT_TRUE(n.at("ulDelays").is_array());
        EXPECT_GE(n.at("ulDelays").size(), 1u);
        EXPECT_GE(n.at("dlDelays").size(), 1u);
        EXPECT_GE(n.at("rtDelays").size(), 1u);
        // rtDelay is ul+dl (a round trip), sanity on the synthesized values.
        EXPECT_EQ(n.at("rtDelays")[0].get<unsigned>(),
                  n.at("ulDelays")[0].get<unsigned>() + n.at("dlDelays")[0].get<unsigned>());
    }
}

TEST(SmfQosMon, PerEventSliceFilterMatchesTheCustomSliceExactly) {
    const json embb{{"sst", 1}};
    const json custom{{"sst", 200}, {"sd", "0a1b2c"}};
    const std::vector<json> sessions{session("imsi-111", embb), session("imsi-222", custom)};
    // Subscription scoped to the custom slice only.
    const std::vector<json> subs{qos_mon_subscription("n1", "https://x/notify", custom)};

    const auto out = smf::qos_mon::build_qos_mon_notifications(subs, sessions, 1, "t");
    ASSERT_EQ(out.size(), 1u);
    const auto& notifs = out[0].body.at("eventNotifs");
    ASSERT_EQ(notifs.size(), 1u) << "only the matching slice should be reported";
    EXPECT_EQ(notifs[0].at("snssai"), custom);
    EXPECT_EQ(notifs[0].at("supi"), "imsi-222");
}

TEST(SmfQosMon, SkipsNonQosMonNoUriAndNoMatch) {
    const std::vector<json> sessions{session("imsi-1", json{{"sst", 1}})};
    const std::vector<json> subs{
        // not a QOS_MON subscriber
        json{{"notifId", "a"}, {"notifUri", "https://x/1"}, {"eventSubs", json::array({{{"event", "PDU_SES_REL"}}})}},
        // QOS_MON but no notifUri
        json{{"notifId", "b"}, {"eventSubs", json::array({{{"event", "QOS_MON"}}})}},
        // QOS_MON scoped to a slice no session has -> no eventNotifs -> skipped
        qos_mon_subscription("c", "https://x/3", json{{"sst", 99}})};

    const auto out = smf::qos_mon::build_qos_mon_notifications(subs, sessions, 1, "t");
    EXPECT_TRUE(out.empty());
}

TEST(SmfQosMon, DelaysAreDeterministicPerTick) {
    const std::vector<json> sessions{session("imsi-1", json{{"sst", 2}})};
    const std::vector<json> subs{qos_mon_subscription("n1", "https://x/notify")};

    const auto a = smf::qos_mon::build_qos_mon_notifications(subs, sessions, 5, "t");
    const auto b = smf::qos_mon::build_qos_mon_notifications(subs, sessions, 5, "t");
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(a[0].body.at("eventNotifs")[0].at("ulDelays"),
              b[0].body.at("eventNotifs")[0].at("ulDelays"));
}
