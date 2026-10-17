// ADR-0276: NSACF (TS 29.536) -- Network Slice Admission Control, both of its real API files.
//
// What this proves beyond "the routes answer": the admission decision itself. The slice under test
// is configured (config/nsacf.json) with a maximum of 10 UEs, so the test admits 10 distinct SUPIs,
// asserts the 11th is REJECTED with the spec's own EXCEED_MAX_UE_NUM reason, then DECREASEs one and
// asserts the next UE is admitted again. A stub that always answered 204 would fail at the 11th.
//
// It also pins the idempotency the store exists for: re-sending INCREASE for a SUPI already on the
// slice must NOT consume another unit of quota. TS 29.536's operations are re-sent in real
// networks (re-registration, restart, retry), and a naive counter would drift upward until the
// slice looked full while holding no UEs.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

// Same helper the UDR onDataChange webhook test uses (ADR-0179): run a real in-process server on
// its own io_context thread so NSACF's notifications have somewhere real to land.
class IoContextThread {
public:
    explicit IoContextThread(boost::asio::io_context& ioc) : ioc_(ioc) {
        thread_ = std::thread([&ioc] { ioc.run(); });
    }
    ~IoContextThread() {
        ioc_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    IoContextThread(const IoContextThread&) = delete;
    IoContextThread& operator=(const IoContextThread&) = delete;

private:
    boost::asio::io_context& ioc_;
    std::thread thread_;
};

constexpr const char* kNsacfBase = "https://127.0.0.1:7797";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

bool wait_reachable(sbi_core::http2::Client& client, const std::string& url, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::string fetch_token(sbi_core::http2::Client& client, const std::string& scope = "nnsacf-nsac") {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=" + scope + "&targetNfType=NSACF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

// The sst=2 slice from config/nsacf.json, whose maximum is 10 UEs -- deliberately the small one,
// so the limit is reachable in a test without 100 requests.
json ue_request(const std::string& supi, const std::string& flag) {
    return json{
        {"ueACRequestInfo",
         json::array(
             {json{{"supi", supi},
                   {"anType", "3GPP_ACCESS"},
                   {"acuOperationList",
                    json::array({json{{"updateFlag", flag}, {"snssai", json{{"sst", 2}}}}})}}})},
        {"nfId", "00000000-0000-4000-8000-0000000000aa"},
    };
}

int post_ue_update(sbi_core::http2::Client& client,
                   const std::string& token,
                   const json& body,
                   std::string& response_body) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string(kNsacfBase) + "/nnsacf-nsac/v1/slices/ues";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = body.dump();
    auto resp = client.send(req);
    if (!resp.has_value()) {
        return -1;
    }
    response_body = resp->body;
    return resp->status;
}

} // namespace

TEST(NsacfIntegration, SliceMaximumIsEnforcedAndReleasedCapacityIsReusable) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    ASSERT_GT(nsacf.pid(), 0) << "failed to fork nsacf";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(client, std::string(kNsacfBase) + "/nnsacf-nsac/v1/slices/ues", 200))
        << "nsacf never became reachable";
    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain an OAuth2 token for NSACF from NRF";

    // The configured maximum for sst=2. Ten distinct UEs must all be admitted (204 = every
    // operation succeeded, the YAML's own "Successful ACU operation").
    constexpr int kMaxUes = 10;
    for (int i = 0; i < kMaxUes; ++i) {
        std::string body;
        const std::string supi = "imsi-99970000000" + std::to_string(1000 + i);
        EXPECT_EQ(post_ue_update(client, token, ue_request(supi, "INCREASE"), body), 204)
            << "UE " << i << " should have been admitted; body: " << body;
    }

    // Idempotency: the first SUPI again. Still 204, and -- proved by the next assertion -- it did
    // not consume a second unit of quota.
    {
        std::string body;
        EXPECT_EQ(
            post_ue_update(client, token, ue_request("imsi-999700000001000", "INCREASE"), body),
            204)
            << "a re-sent INCREASE for a UE already on the slice must still succeed";
    }

    // The 11th distinct UE: the slice is full. 200 with the failure list, not 204.
    {
        std::string body;
        const int status =
            post_ue_update(client, token, ue_request("imsi-999700000009999", "INCREASE"), body);
        ASSERT_EQ(status, 200) << "expected a partial-success 200 carrying acuFailureList, not "
                               << status << "; body: " << body;
        const auto parsed = json::parse(body);
        ASSERT_TRUE(parsed.contains("acuFailureList")) << body;
        ASSERT_FALSE(parsed.at("acuFailureList").empty()) << body;
        EXPECT_EQ(parsed.at("acuFailureList")[0].at("reason").get<std::string>(),
                  "EXCEED_MAX_UE_NUM")
            << "the spec's own reason for this rejection; body: " << body;
        EXPECT_EQ(parsed.at("acuFailureList")[0].at("snssai").at("sst").get<int>(), 2);
    }

    // Release one, and the slice has room again. This is what distinguishes a real admission
    // control from a request counter that only ever climbs.
    {
        std::string body;
        EXPECT_EQ(
            post_ue_update(client, token, ue_request("imsi-999700000001000", "DECREASE"), body),
            204);
        EXPECT_EQ(
            post_ue_update(client, token, ue_request("imsi-999700000009999", "INCREASE"), body),
            204)
            << "after a DECREASE freed a unit of quota, the previously-rejected UE must be "
               "admitted; body: "
            << body;
    }
}

TEST(NsacfIntegration, QuotaQueryAndLocalNumberUpdateAreReal) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    ASSERT_GT(nsacf.pid(), 0);

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, std::string(kNsacfBase) + "/nnsacf-nsac/v1/slices/ues", 200));
    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty());

    // Explicit return type: resp->status is a long, and -1 is an int, which the compiler will
    // not deduce a common type for.
    auto query = [&](int sst, const std::string& quota_type, std::string& out_body) -> long {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = std::string(kNsacfBase) + "/nnsacf-nsac/v1/slices/roaming-quotas/query";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json{{"snssai", json{{"sst", sst}}},
                        {"plmnId", json{{"mcc", "999"}, {"mnc", "70"}}},
                        {"quotaType", quota_type}}
                       .dump();
        auto resp = client.send(req);
        if (!resp.has_value()) {
            return -1;
        }
        out_body = resp->body;
        return resp->status;
    };

    // BOTH returns both maxima, from config.
    std::string body;
    ASSERT_EQ(query(2, "BOTH", body), 200) << body;
    auto parsed = json::parse(body);
    EXPECT_EQ(parsed.at("maxUesNumber").get<int>(), 10);
    EXPECT_EQ(parsed.at("maxPdusNumber").get<int>(), 20);

    // MAX_UE_NUM returns only that one -- the quotaType really selects, rather than the answer
    // always carrying everything.
    ASSERT_EQ(query(2, "MAX_UE_NUM", body), 200) << body;
    parsed = json::parse(body);
    EXPECT_TRUE(parsed.contains("maxUesNumber"));
    EXPECT_FALSE(parsed.contains("maxPdusNumber"))
        << "quotaType=MAX_UE_NUM must not answer with the PDU maximum too; body: " << body;

    // A slice this NSACF has no configuration for is a real 404, not a zero quota.
    EXPECT_EQ(query(99, "BOTH", body), 404) << body;

    // LocalNumberUpdate changes the maximum for real, and a request carrying only one of the two
    // fields must leave the other alone.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = std::string(kNsacfBase) + "/nnsacf-nsac/v1/slices/local-configs/update";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json{{"snssai", json{{"sst", 2}}}, {"maxUesNumber", 42}}.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 204) << resp->body;
    }
    ASSERT_EQ(query(2, "BOTH", body), 200) << body;
    parsed = json::parse(body);
    EXPECT_EQ(parsed.at("maxUesNumber").get<int>(), 42) << "LocalNumberUpdate did not take effect";
    EXPECT_EQ(parsed.at("maxPdusNumber").get<int>(), 20)
        << "an update carrying only maxUesNumber must not zero the PDU maximum; body: " << body;
}

// ADR-0276: the two callbacks the YAML defines, proved by receiving them.
//
// Both are real callbacks in TS 29.536, not extras: `eventReport` on CreateSubscription posts a
// SACEventReport to the subscription's own eventNotifyUri, and `eacNotification` on NumOfUEsUpdate
// posts an EacNotification to the eacNotificationUri the requesting NF supplied. A NSACF that
// stored subscriptions and never published would pass every CRUD assertion and still be useless to
// the AMF that subscribed -- which is exactly why this test receives them on a real server rather
// than asserting on NSACF's own 201.
TEST(NsacfIntegration, ThresholdReportAndEacNotificationAreReallyDelivered) {
    boost::asio::io_context receiver_ioc;
    sbi_core::http2::TlsConfig receiver_tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Server receiver(receiver_ioc, "127.0.0.1", 19998, receiver_tls);

    std::mutex received_mutex;
    std::vector<json> reports;
    std::vector<json> eac_notifications;
    receiver.add_route(
        "POST", "/sac-report", [&received_mutex, &reports](const sbi_core::http2::Request& req) {
            std::lock_guard<std::mutex> lock(received_mutex);
            reports.push_back(json::parse(req.body));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    receiver.add_route(
        "POST", "/eac", [&received_mutex, &eac_notifications](const sbi_core::http2::Request& req) {
            std::lock_guard<std::mutex> lock(received_mutex);
            eac_notifications.push_back(json::parse(req.body));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    receiver.start();
    IoContextThread receiver_thread(receiver_ioc);

    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    ASSERT_GT(nsacf.pid(), 0);

    auto client = make_client();
    ASSERT_TRUE(
        wait_reachable(client, std::string(kNsacfBase) + "/nnsacf-nsac/v1/slices/ues", 200));
    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty());
    const std::string ee_token = fetch_token(client, "nnsacf-slice-ee"); // ADR-0499: per service
    ASSERT_FALSE(ee_token.empty());

    // Subscribe to NUM_OF_REGD_UES on the sst=2 slice with a THRESHOLD of 5 UEs, and ask for an
    // immediate report so the creation response itself is checked too.
    std::string subscription_id;
    {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = std::string(kNsacfBase) + "/nnsacf-slice-ee/v1/subscriptions";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + ee_token);
        req.body =
            json{
                {"event",
                 json{{"eventType", "NUM_OF_REGD_UES"},
                      {"eventTrigger", "THRESHOLD"},
                      {"eventFilter", json::array({json{{"sst", 2}}})},
                      {"notifThreshold", json{{"numericValNumUes", 5}}},
                      {"immediateFlag", true}}},
                {"eventNotifyUri", "https://127.0.0.1:19998/sac-report"},
                {"nfId", "00000000-0000-4000-8000-0000000000aa"},
                {"notifyCorrelationId", "corr-42"},
            }
                .dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 201) << resp->body;

        // The 201 body must be CreatedSACEventSubscription -- {subscription, subscriptionId} --
        // not the request echoed back. An earlier draft of this NF returned the raw request, which
        // the schema does not permit.
        const auto created = json::parse(resp->body);
        ASSERT_TRUE(created.contains("subscriptionId")) << resp->body;
        ASSERT_TRUE(created.contains("subscription")) << resp->body;
        EXPECT_EQ(created.at("subscription").at("eventNotifyUri").get<std::string>(),
                  "https://127.0.0.1:19998/sac-report");
        // immediateFlag asked for a report in the creation response itself.
        ASSERT_TRUE(created.contains("report")) << "immediateFlag was set; " << resp->body;
        EXPECT_EQ(created.at("report").at("eventType").get<std::string>(), "NUM_OF_REGD_UES");
        subscription_id = created.at("subscriptionId").get<std::string>();
    }
    ASSERT_FALSE(subscription_id.empty());

    // Five UEs reaches the threshold, and the 8-of-10 UE crosses the 80% EAC activation point
    // configured in config/nsacf.json.
    for (int i = 0; i < 8; ++i) {
        std::string body;
        const std::string supi = "imsi-99970000000" + std::to_string(2000 + i);
        json request = ue_request(supi, "INCREASE");
        request["eacNotificationUri"] = "https://127.0.0.1:19998/eac";
        EXPECT_EQ(post_ue_update(client, token, request, body), 204) << body;
    }

    // Delivery is fire-and-forget from NSACF's own request thread, so allow it to land.
    for (int attempt = 0; attempt < 50; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(received_mutex);
            if (!reports.empty() && !eac_notifications.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::lock_guard<std::mutex> lock(received_mutex);
    ASSERT_FALSE(reports.empty())
        << "no SACEventReport was delivered -- the subscription's own eventNotifyUri callback, "
           "which is what makes this service more than a CRUD store";
    const auto& report = reports.front();
    ASSERT_TRUE(report.contains("report")) << report.dump();
    EXPECT_EQ(report.at("report").at("eventType").get<std::string>(), "NUM_OF_REGD_UES");
    EXPECT_EQ(report.at("report").at("eventFilter").at("sst").get<int>(), 2);
    EXPECT_EQ(report.at("notifyCorrelationId").get<std::string>(), "corr-42")
        << "the subscription's notifyCorrelationId must come back on the report";
    // sliceStautsInfo carries the count that triggered it -- at or past the threshold of 5.
    ASSERT_TRUE(report.at("report").contains("sliceStautsInfo")) << report.dump();
    EXPECT_GE(report.at("report")
                  .at("sliceStautsInfo")
                  .at("reachedNumUes")
                  .at("numericValNumUes")
                  .get<int>(),
              5);

    ASSERT_FALSE(eac_notifications.empty())
        << "no EacNotification was delivered after the slice passed its 80% activation point";
    const auto& eac = eac_notifications.front();
    ASSERT_TRUE(eac.contains("eacModeList")) << eac.dump();
    EXPECT_EQ(eac.at("eacModeList").at("2").get<std::string>(), "ACTIVE")
        << "the slice crossed its EAC activation threshold, so its mode must be ACTIVE; "
        << eac.dump();
}
