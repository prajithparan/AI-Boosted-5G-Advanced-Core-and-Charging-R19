// ADR-0519 (REVIEW-5ae0ef1 SPEC-01, increment 1): the NEF's AsSessionWithQoS -> PCF mapping for
// TS 23.502 4.15.6.6 steps 3 and 6 and 4.15.6.6a, against TS 29.522 4.4.9.2's event rules. The
// request and update bodies are written as samples for validate_structural_conformance.py
// (AppSessionContextReqData, AppSessionContextUpdateDataPatch in TS29514_Npcf_PolicyAuthorization).
// ADR-0520 (increment 2): the PCF's EventsNotification -> the AF's UserPlaneEventReports, TS 23.502
// 4.15.6.6 steps 7-8 and TS 29.522 4.4.9.2.2; sample UserPlaneNotificationData (TS29122).

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "qos_mapping.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

void write_sample(const std::string& name, const json& j) {
    const char* dir = std::getenv("SBI_CONFORMANCE_SAMPLES_DIR");
    const std::filesystem::path d = dir != nullptr ? dir : ".";
    std::filesystem::create_directories(d);
    std::ofstream(d / name) << j.dump(2);
}

sbi_gen::AsSessionWithQoSSubscription af_subscription() {
    return json{
        {"notificationDestination", "https://af.example.com/qos-events"},
        {"ueIpv4Addr", "10.45.0.7"},
        {"dnn", "internet"},
        {"snssai", {{"sst", 1}, {"sd", "000001"}}},
        {"qosReference", "qos-gold"},
        {"flowInfo",
         json::array({json{{"flowId", 1},
                           {"flowDescriptions",
                            json::array({"permit out ip from 10.45.0.7 to 198.51.100.10"})}},
                      json{{"flowId", 2},
                           {"flowDescriptions",
                            json::array({"permit in ip from 198.51.100.10 to 10.45.0.7"})}}})},
        {"events", json::array({"QOS_GUARANTEED", "QOS_NOT_GUARANTEED", "ACCESS_TYPE_CHANGE"})},
        {"supportedFeatures", "ff"}}
        .get<sbi_gen::AsSessionWithQoSSubscription>();
}

std::vector<std::string> names(const json& events) {
    std::vector<std::string> out;
    for (const auto& e : events) {
        out.push_back(e.at("event").get<std::string>());
    }
    return out;
}

TEST(NefQosMapping, ResourceAllocationIsAlwaysSubscribedAndQosEventsBecomeOneQosNotif) {
    const auto plan = nef::pcf_events_for(af_subscription());
    EXPECT_EQ(plan.pcf_events,
              (std::vector<std::string>{"SUCCESSFUL_RESOURCES_ALLOCATION",
                                        "FAILED_RESOURCES_ALLOCATION",
                                        "QOS_NOTIF",
                                        "ACCESS_TYPE_CHANGE"}));
    EXPECT_TRUE(plan.not_subscribed.empty());
}

TEST(NefQosMapping, AUsageThresholdSubscribesUsageReportAndUnmappedEventsAreListed) {
    auto sub = af_subscription();
    sub.events = std::vector<sbi_gen::UserPlaneEvent>{{"QOS_MONITORING"}, {"SESSION_TERMINATION"}};
    sub.usageThreshold = json{{"totalVolume", 1000000}}.get<sbi_gen::UsageThreshold>();
    const auto plan = nef::pcf_events_for(sub);
    EXPECT_EQ(plan.pcf_events,
              (std::vector<std::string>{"SUCCESSFUL_RESOURCES_ALLOCATION",
                                        "FAILED_RESOURCES_ALLOCATION",
                                        "USAGE_REPORT"}));
    // SESSION_TERMINATION needs no subscription; QOS_MONITORING is not mapped.
    EXPECT_EQ(plan.not_subscribed, std::vector<std::string>{"QOS_MONITORING"});
}

TEST(NefQosMapping, CreateCarriesTheUeAddressFlowsQosReferenceAndEvents) {
    const json req = nef::app_session_request(af_subscription(), "https://nef:7790/cb/af1/s1");
    write_sample("nef_pcf_app_session_req.json", req);

    EXPECT_EQ(req.at("ueIpv4"), "10.45.0.7");
    EXPECT_EQ(req.at("dnn"), "internet");
    EXPECT_EQ(req.at("sliceInfo").at("sd"), "000001");
    EXPECT_EQ(req.at("notifUri"), "https://nef:7790/cb/af1/s1");
    EXPECT_EQ(req.at("suppFeat"), "0") << "the AF's TS 29.122 feature bits are not Npcf bits";
    const auto& mc = req.at("medComponents").at("1");
    EXPECT_EQ(mc.at("medCompN"), 1);
    EXPECT_EQ(mc.at("qosReference"), "qos-gold");
    EXPECT_EQ(mc.at("medSubComps").at("2").at("fNum"), 2);
    EXPECT_EQ(mc.at("medSubComps").at("2").at("fDescs"),
              json::array({"permit in ip from 198.51.100.10 to 10.45.0.7"}));
    EXPECT_EQ(names(req.at("evSubsc").at("events")),
              (std::vector<std::string>{"SUCCESSFUL_RESOURCES_ALLOCATION",
                                        "FAILED_RESOURCES_ALLOCATION",
                                        "QOS_NOTIF",
                                        "ACCESS_TYPE_CHANGE"}));
}

TEST(NefQosMapping, AnUpdateRemovesAFlowAndAQosReferenceAtThePcf) {
    const auto before = af_subscription();
    auto after = before;
    after.flowInfo->pop_back(); // flow 2 removed
    after.qosReference.reset();

    const auto from = nef::pcf_session_state(before);
    const auto to = nef::pcf_session_state(after);
    const auto patch = nef::pcf_update_patch(before, after);
    write_sample("nef_pcf_app_session_update.json", json{{"ascReqData", patch}});
    // MediaComponentRm / MediaSubComponentRm require their number even when unchanged.
    EXPECT_EQ(patch.at("medComponents").at("1").at("medCompN"), 1);

    EXPECT_TRUE(patch.at("medComponents").at("1").at("medSubComps").at("2").is_null());
    EXPECT_TRUE(patch.at("medComponents").at("1").at("qosReference").is_null());
    EXPECT_FALSE(patch.contains("evSubsc")) << "unchanged members are not sent";

    // RFC 7396: applying the patch to what the PCF holds gives what the AF now asks for.
    json applied = from;
    applied.merge_patch(patch);
    EXPECT_EQ(applied, to);
}

TEST(NefQosMapping, AnUnchangedSubscriptionSendsNoUpdate) {
    const auto sub = af_subscription();
    EXPECT_TRUE(nef::pcf_update_patch(sub, sub).empty());
}

std::vector<std::string> report_names(const std::vector<sbi_gen::UserPlaneEventReport>& reports) {
    std::vector<std::string> out;
    for (const auto& r : reports) {
        out.push_back(r.event.value);
    }
    return out;
}

TEST(NefQosMapping, TheCreateGivesThePcfTheNefCallbackForEvents) {
    const json req = nef::app_session_request(af_subscription(), "https://nef/cb/af-1/1");
    EXPECT_EQ(req.at("notifUri"), "https://nef/cb/af-1/1");
    EXPECT_EQ(req.at("evSubsc").at("notifUri"), "https://nef/cb/af-1/1");
}

TEST(NefQosMapping, PcfEventsBecomeTheAfsUserPlaneEventReports) {
    const auto n = json{
        {"evSubsUri", "https://pcf/npcf-policyauthorization/v1/app-sessions/7/events-subscription"},
        {"evNotifs",
         json::array({json{{"event", "SUCCESSFUL_RESOURCES_ALLOCATION"}},
                      json{{"event", "QOS_NOTIF"}},
                      json{{"event", "USAGE_REPORT"}},
                      json{{"event", "ACCESS_TYPE_CHANGE"}},
                      json{{"event", "PLMN_CHG"}},
                      json{{"event", "APP_DETECTION"}}})},
        {"succResourcAllocReports",
         json::array({json{
             {"flows", json::array({json{{"medCompN", 1}, {"fNums", json::array({1, 2})}}})}}})},
        {"qncReports",
         json::array(
             {json{{"notifType", "NOT_GUARANTEED"},
                   {"flows", json::array({json{{"medCompN", 1}, {"fNums", json::array({2})}}})},
                   {"altSerReqNotSuppInd", true}},
              json{{"notifType", "GUARANTEED"}}})},
        {"usgRep", json{{"totalVolume", 1000}}},
        {"ratType", "NR"},
        {"plmnId",
         json{{"mcc", "001"},
              {"mnc",
               "01"}}}}.get<sbi_gen::EventsNotification_Npcf_PolicyAuthorization>();

    const auto plan = nef::af_reports_for(n);
    ASSERT_EQ(report_names(plan.reports),
              (std::vector<std::string>{"SUCCESSFUL_RESOURCES_ALLOCATION",
                                        "QOS_NOT_GUARANTEED",
                                        "QOS_GUARANTEED",
                                        "USAGE_REPORT",
                                        "ACCESS_TYPE_CHANGE",
                                        "PLMN_CHG"}));
    EXPECT_EQ(plan.not_mapped, (std::vector<std::string>{"APP_DETECTION"}));

    const auto& r = plan.reports;
    EXPECT_EQ(r[0].flowIds, (std::vector<std::int64_t>{1, 2})) << "fNums are the AF's flowIds";
    EXPECT_EQ(r[1].flowIds, (std::vector<std::int64_t>{2}));
    EXPECT_EQ(r[1].altQosNotSuppInd, true);
    EXPECT_FALSE(r[2].flowIds.has_value()) << "no flow numbers: the event applies to all flows";
    EXPECT_EQ(json(*r[3].accumulatedUsage).at("totalVolume"), 1000);
    EXPECT_EQ(*r[4].ratType, "NR");
    EXPECT_EQ(json(*r[5].plmnId).at("mcc"), "001");

    sbi_gen::UserPlaneNotificationData out{};
    out.transaction = "https://nef/3gpp-as-session-with-qos/v1/af-1/subscriptions/1";
    out.eventReports = plan.reports;
    out.eventReports.push_back(nef::session_termination_report());
    write_sample("nef_af_user_plane_notification.json", json(out));
    EXPECT_EQ(json(out).at("eventReports").back().at("event"), "SESSION_TERMINATION");
}

TEST(NefQosMapping, AQosNotifWithoutAQosReportHasNoAfEvent) {
    const auto n = json{{"evSubsUri", "https://pcf/e"},
                        {"evNotifs", json::array({json{{"event", "QOS_NOTIF"}}})}}
                       .get<sbi_gen::EventsNotification_Npcf_PolicyAuthorization>();
    const auto plan = nef::af_reports_for(n);
    EXPECT_TRUE(plan.reports.empty());
    EXPECT_EQ(plan.not_mapped, (std::vector<std::string>{"QOS_NOTIF without qncReports"}));
}

} // namespace
