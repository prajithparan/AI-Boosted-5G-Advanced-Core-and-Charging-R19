// ADR-0489: the NWDAF IRI-POI (TS 33.128 clause 7.16) at the REAL nwdaf process. The test is the NF
// consumer (Nnwdaf_EventsSubscription subscribe / update / unsubscribe, Nnwdaf_AnalyticsInfo
// request) and the notification receiver; the warrant is provisioned over real LI_X1 and the xIRIs
// are read off a loopback MDF2's real LI_X2 server and decoded with the MDF2's own codec.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "li_core/x1.hpp"
#include "li_core/x2x3_pdu.hpp"
#include "li_core/x2x3_server.hpp"
#include "li_core/xiri.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;
namespace xiri = li_core::xiri;

constexpr const char* kTargetImsi = "001010000000091";
constexpr const char* kOtherImsi = "001010000000092";
constexpr std::uint16_t kX1Port = 19864;
constexpr const char* kX1Url = "https://127.0.0.1:19864/X1/NE";
constexpr const char* kNeId = "nwdaf-poi-e2e";
constexpr const char* kNwdaf = "https://127.0.0.1:7798";
constexpr const char* kSubsRoot = "/nnwdaf-eventssubscription/v1";
constexpr std::uint16_t kNotifyPort = 19995;

class Inbox {
public:
    void push(li_core::Pdu v) {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(v));
        cv_.notify_all();
    }
    template <typename Pred> bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return pred(items_); });
    }
    std::vector<li_core::Pdu> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<li_core::Pdu> items_;
};

li_core::X2X3ServerConfig fake_mdf2_config() {
    li_core::X2X3ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;
    cfg.server_cert_path = CERTS_DIR "/nwdaf/cert.pem";
    cfg.server_key_path = CERTS_DIR "/nwdaf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

// config/nwdaf.json with li_poi enabled toward the loopback MDF2, a 2 s notification interval and
// no DCCF collection (not what is under test).
std::string write_config(std::uint16_t mdf2_port) {
    std::ifstream in(NWDAF_CONFIG_TEMPLATE);
    json cfg = json::parse(in);
    cfg["notification_interval_seconds"] = 2;
    cfg["data_collection"]["via_dccf"] = false;
    auto& lp = cfg["li_poi"];
    lp["enabled"] = true;
    lp["x1_bind_address"] = "127.0.0.1";
    lp["x1_port"] = kX1Port;
    lp["ne_identifier"] = kNeId;
    lp["mdf2_host"] = "127.0.0.1";
    lp["mdf2_port"] = mdf2_port;
    lp["mdf2_sni"] = "localhost";
    const std::string path = std::string(::testing::TempDir()) + "nwdaf_li_e2e.json";
    std::ofstream(path) << cfg.dump(2);
    return path;
}

sbi_core::http2::TlsConfig test_tls() {
    return sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
}

std::string x1_activate(const std::string& xid) {
    namespace x1 = li_core::x1;
    x1::TaskDetails task;
    task.xid = xid;
    task.targets.push_back({x1::TargetIdentifierKind::SupiImsi, "supiimsi", kTargetImsi});
    task.delivery = x1::DeliveryType::X2Only;
    task.dids = {"33333333-3333-4333-8333-333333333333"};
    x1::Request request;
    request.header = {"admf-test",
                      kNeId,
                      "2026-10-14T00:00:00.000000Z",
                      "v1.23.1",
                      "00000000-0000-4000-8000-0000000000f3"};
    request.type = x1::MessageType::ActivateTask;
    request.body = x1::ActivateTask{task};
    const auto xml = x1::serialise_request({request});
    return xml.has_value() ? *xml : std::string();
}

bool post_x1(sbi_core::http2::Client& admf, const std::string& body) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = kX1Url;
    req.headers.emplace("content-type", "application/xml");
    req.body = body;
    for (int attempt = 0; attempt < 150; ++attempt) {
        if (const auto r = admf.send(req); r.has_value()) {
            return r->status == 200 && r->body.find("ErrorResponse") == std::string::npos;
        }
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

std::string url_encode(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

sbi_core::http2::ClientResponse consumer_request(const std::string& method,
                                                 const std::string& url,
                                                 const json& body,
                                                 const std::string& user_agent) {
    sbi_core::http2::Client client(test_tls());
    nf_test::attach_test_tokens(client); // ADR-0499: the NWDAF enforces tokens
    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    if (!user_agent.empty()) {
        req.headers.emplace("user-agent", user_agent);
    }
    if (!body.is_null()) {
        req.headers.emplace("content-type", "application/json");
        req.body = body.dump();
    }
    for (int attempt = 0; attempt < 150; ++attempt) {
        if (auto r = client.send(req); r.has_value()) {
            return *r;
        }
        std::this_thread::sleep_for(100ms);
    }
    ADD_FAILURE() << method << " " << url << " never answered";
    return {};
}

json target_ue(std::vector<std::string> supis) {
    json arr = json::array();
    for (const auto& s : supis) {
        arr.push_back("imsi-" + s);
    }
    return json{{"supis", arr}};
}

template <typename T> std::vector<T> records(const std::vector<li_core::Pdu>& pdus) {
    std::vector<T> out;
    for (const auto& pdu : pdus) {
        if (const auto d = xiri::decode_xiri_payload(pdu.payload); d.has_value()) {
            if (const auto* e = std::get_if<T>(&d->event)) {
                out.push_back(*e);
            }
        }
    }
    return out;
}

template <typename T> std::size_t count(const std::vector<li_core::Pdu>& pdus) {
    return records<T>(pdus).size();
}

struct Lab {
    Inbox mdf2;
    li_core::X2X3Server mdf2_server{
        fake_mdf2_config(), [this](const li_core::Pdu& pdu, std::string_view) { mdf2.push(pdu); }};
};

} // namespace

TEST(LiNwdafEndToEnd, SubscribeNotifyUpdateUnsubscribeAndQueryForATargetAreReported) {
    // The consumer's notification endpoint.
    boost::asio::io_context receiver_ioc;
    sbi_core::http2::Server receiver(receiver_ioc, "127.0.0.1", kNotifyPort, test_tls());
    receiver.add_route("POST", "/nwdaf-notify", [](const sbi_core::http2::Request&) {
        sbi_core::http2::Response resp;
        resp.status = 204;
        return resp;
    });
    receiver.start();
    std::thread receiver_thread([&receiver_ioc] { receiver_ioc.run(); });

    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port());
    ::setenv("NWDAF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess nwdaf{NWDAF_PATH};
    ::unsetenv("NWDAF_CONFIG_FILE");
    ASSERT_GT(nwdaf.pid(), 0);

    sbi_core::http2::Client admf(test_tls());
    ASSERT_TRUE(post_x1(admf, x1_activate("f2e1e2e3-e4e5-46e7-88e9-eaebecedeeef")));

    // A UE-related analytic for the target alone, plus NF_LOAD (not of LI interest).
    const json sub{
        {"eventSubscriptions",
         json::array({json{{"event", "ABNORMAL_BEHAVIOUR"}, {"tgtUe", target_ue({kTargetImsi})}},
                      json{{"event", "NF_LOAD"}}})},
        {"notificationURI", "https://127.0.0.1:" + std::to_string(kNotifyPort) + "/nwdaf-notify"}};
    const auto created = consumer_request(
        "POST", std::string(kNwdaf) + kSubsRoot + "/subscriptions", sub, "PCF-e2e");
    ASSERT_EQ(created.status, 201) << created.body;
    const std::string location = created.headers.find("location")->second;

    // The notifier runs every 2 s: wait for one notification record.
    EXPECT_TRUE(lab.mdf2.wait_until(
        [](const auto& v) { return count<xiri::NwdafEventsNotification>(v) >= 1; }, 15s))
        << "no NWDAFEventsNotification";

    json updated = sub;
    updated["eventSubscriptions"][0]["event"] = "UE_MOBILITY";
    ASSERT_EQ(consumer_request("PUT", std::string(kNwdaf) + location, updated, "PCF-e2e").status,
              200);
    ASSERT_EQ(consumer_request("DELETE", std::string(kNwdaf) + location, json(), "PCF-e2e").status,
              204);

    const std::string query = std::string(kNwdaf) +
                              "/nnwdaf-analyticsinfo/v1/analytics?event-id=ABNORMAL_BEHAVIOUR" +
                              "&tgt-ue=" + url_encode(target_ue({kTargetImsi}).dump());
    const auto answered = consumer_request("GET", query, json(), "NEF-e2e");

    ASSERT_TRUE(lab.mdf2.wait_until(
        [](const auto& v) {
            return count<xiri::NwdafEventsSubscription>(v) >= 3 &&
                   count<xiri::NwdafAnalyticsInfoQuery>(v) >= 1;
        },
        15s))
        << "expected three NWDAFEventsSubscription and one NWDAFAnalyticsInfoQuery";
    receiver_ioc.stop();
    receiver_thread.join();

    const auto pdus = lab.mdf2.take();
    const auto subs = records<xiri::NwdafEventsSubscription>(pdus);
    ASSERT_EQ(subs.size(), 3U);
    using Op = xiri::NwdafEventsSubscriptionOpType;
    using Code = xiri::NwdafEventsSubscriptionResponseCode;
    const std::vector<std::tuple<Op, Code, xiri::NwdafEvent>> expected{
        {Op::Post, Code::Created201, xiri::NwdafEvent::AbnormalBehaviour},
        {Op::Put, Code::Ok200, xiri::NwdafEvent::UeMobility},
        // DELETE reports the subscription as it stood: the update's UE_MOBILITY.
        {Op::Delete, Code::NoContent204, xiri::NwdafEvent::UeMobility}};
    for (std::size_t i = 0; i < subs.size(); ++i) {
        const auto& r = subs[i];
        EXPECT_EQ(std::get<xiri::Imsi>(r.supi).digits, kTargetImsi);
        EXPECT_EQ(r.consumer_nf_type, xiri::NwdafConsumerNfType::Pcf);
        EXPECT_EQ(r.op_type, std::get<0>(expected[i])) << "record " << i;
        EXPECT_EQ(r.response_code, std::get<1>(expected[i])) << "record " << i;
        // NF_LOAD is not a UE-related analytic: only the target's event is listed.
        EXPECT_EQ(r.subscribed_events, std::vector{std::get<2>(expected[i])}) << "record " << i;
        EXPECT_EQ(r.subscription.reference,
                  "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/"
                  "NnwdafEventsSubscription");
        ASSERT_TRUE(r.subscription_id.has_value()) << "record " << i;
        EXPECT_EQ(*r.subscription_id, location);
    }
    EXPECT_EQ(json::parse(subs[0].subscription.value), sub);
    EXPECT_EQ(json::parse(subs[1].subscription.value), updated);

    const auto notes = records<xiri::NwdafEventsNotification>(pdus);
    ASSERT_GE(notes.size(), 1U);
    EXPECT_EQ(std::get<xiri::Imsi>(notes[0].supi).digits, kTargetImsi);
    EXPECT_EQ(notes[0].notification.reference,
              "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/"
              "NnwdafEventsSubscriptionNotification");
    const auto note_body = json::parse(notes[0].notification.value);
    EXPECT_EQ(note_body.at("subscriptionId"), location.substr(location.rfind('/') + 1));

    const auto queries = records<xiri::NwdafAnalyticsInfoQuery>(pdus);
    ASSERT_EQ(queries.size(), 1U);
    EXPECT_EQ(std::get<xiri::Imsi>(queries[0].supi).digits, kTargetImsi);
    EXPECT_EQ(queries[0].event, xiri::NwdafEvent::AbnormalBehaviour);
    EXPECT_EQ(queries[0].response_code,
              xiri::to_nwdaf_analytics_info_response_code(static_cast<int>(answered.status)));
    // AnalyticsData only with a 200.
    EXPECT_EQ(queries[0].analytics_data.has_value(), answered.status == 200);
    lab.mdf2_server.stop();
}

TEST(LiNwdafEndToEnd, UnknownConsumerMultiSupiAndNonTargetProduceNoRecord) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port());
    ::setenv("NWDAF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess nwdaf{NWDAF_PATH};
    ::unsetenv("NWDAF_CONFIG_FILE");

    sbi_core::http2::Client admf(test_tls());
    ASSERT_TRUE(post_x1(admf, x1_activate("f3e1e2e3-e4e5-46e7-88e9-eaebecedeeef")));
    const std::string url = std::string(kNwdaf) + kSubsRoot + "/subscriptions";
    const auto ab = [](const json& tgt) {
        return json{{"eventSubscriptions",
                     json::array({json{{"event", "ABNORMAL_BEHAVIOUR"}, {"tgtUe", tgt}}})}};
    };
    // No User-Agent: nWDAFConsumerNFType is unknown -> withheld (ADR-0489).
    EXPECT_EQ(consumer_request("POST", url, ab(target_ue({kTargetImsi})), "").status, 201);
    // A User-Agent naming an NF type outside the ENUMERATED -> withheld.
    EXPECT_EQ(consumer_request("POST", url, ab(target_ue({kTargetImsi})), "UDM-e2e").status, 201);
    // Two SUPIs: the analytics is not run for the target alone (7.16.2.1.1).
    EXPECT_EQ(
        consumer_request("POST", url, ab(target_ue({kTargetImsi, kOtherImsi})), "PCF-e2e").status,
        201);
    // A non-target SUPI.
    EXPECT_EQ(consumer_request("POST", url, ab(target_ue({kOtherImsi})), "PCF-e2e").status, 201);
    EXPECT_FALSE(lab.mdf2.wait_until([](const auto& v) { return !v.empty(); }, 3s))
        << "a withheld / non-matching request produced an xIRI";
    lab.mdf2_server.stop();
}
