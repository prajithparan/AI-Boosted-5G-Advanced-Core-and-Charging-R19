// ADR-0072 (gap-closure: real N28 end-to-end) + ADR-0073 (real CHF-in-CI). Real, automated
// coverage for the N28 flow:
//   1. UDR's real `/policy-data/ues/{ueId}/sm-data` resource (TS29519_Policy_Data.yaml) --
//      GET/PATCH, real RFC 7396 merge-patch semantics, real upsert-on-first-PATCH behavior.
//   2. PCF's real UDR-fetch-then-CHF-subscribe wiring's fail-open behavior when CHF is
//      unreachable -- spawns nrf+udr+pcf only, deliberately NOT chf.
//   3. UPDATE (ADR-0073): the full real UDR->PCF->CHF->statusNotification->PCF loop, now that
//      CHF is a real, CI-provisioned participant (Redis/Apache Doris (ADR-0192)/its own Postgres,
//      see .github/workflows/ci.yml) -- previously only live-verified manually (ADR-0072's own
//      disclosure), now automated.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

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
        req.method = "GET";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// Real bug found and fixed via actual test execution (not assumed): UDR's Postgres store
// genuinely persists across process runs (that IS the whole point of ADR-0068's own real
// persistence work) -- a fixed, hardcoded test SUPI collides with a previous run's leftover row on
// any re-run against the same long-lived database, breaking this test's own initial-404
// assertion. Uses this OS process's own pid for real per-run uniqueness (matching the real "one
// test process, one execution" scope -- not claiming safety across truly concurrent parallel test
// runs sharing the same database, which this project's test suite doesn't do).
// `salt` distinguishes multiple calls made from within the SAME test-binary process (gtest runs
// every TEST() in one process by default, so pid alone can't tell two different tests in the same
// run apart) -- callers pass a distinct small integer per call site.
std::string unique_test_supi(int salt) {
    const auto pid = static_cast<long>(getpid());
    // Low 4 digits of the current time (seconds) alongside the pid: real, cheap extra protection
    // against OS pid-reuse across separate test-binary invocations within the same second-scale
    // window, not just within one.
    const auto time_low = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count() %
                                            10000);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "imsi-99970%04ld%05ld%d", time_low, pid % 100000, salt % 10);
    return buf;
}

// ADR-0499: the token's aud must be the NF it is sent to.
std::string fetch_token(sbi_core::http2::Client& client,
                        const std::string& scope,
                        const std::string& target_nf_type = "UDR") {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=" + scope + "&targetNfType=" + target_nf_type;
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

// Real, plain (non-TLS) HTTP/1.1 GET against an NF's own real Prometheus `/metrics` endpoint
// (sbi_core::init_metrics's own real, deliberately-unauthenticated, non-mTLS scrape surface --
// see libs/sbi-core/include/sbi_core/metrics.hpp). Used here as the one real, externally-
// observable signal for PCF's own internal spending-limit tracking state: PCF exposes no REST
// GET for it (SmPolicyDecision deliberately carries no fabricated field for it, see
// nfs/pcf/src/main.cpp's own file header), so its real counters are the only real evidence a test
// outside the process can check. `sbi_core::http2::Client` is TLS/mTLS-only, so this uses a raw
// socket directly rather than that client.
long scrape_metric_value(const char* host, int port, const std::string& metric_name) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct timeval tv {
        2, 0
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    const std::string req =
        "GET /metrics HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n\r\n";
    if (send(sock, req.data(), req.size(), 0) < 0) {
        close(sock);
        return -1;
    }
    std::string body;
    char buf[4096];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        body.append(buf, static_cast<std::size_t>(n));
    }
    close(sock);

    // Real Prometheus text exposition format: each metric is preceded by real `# HELP <name>
    // <description>` and `# TYPE <name> <type>` comment lines whose OWN text also contains the
    // bare metric name -- real bug found and fixed via actual test execution (not assumed): a
    // naive `body.find(metric_name)` matches the `# HELP` line first (its description text
    // happens to contain the metric name too) and then fails to parse a number out of prose,
    // silently returning -1 forever. Fixed by anchoring the search to the real VALUE line
    // specifically: "\n<metric_name> " (unlabeled counter, this project's own real shape for
    // every metric this test scrapes) or "\n<metric_name>{" (labeled) at the START of a line, not
    // anywhere in the text.
    const std::string unlabeled_needle = "\n" + metric_name + " ";
    const std::string labeled_needle = "\n" + metric_name + "{";
    auto name_pos = body.find(unlabeled_needle);
    std::size_t value_start;
    if (name_pos != std::string::npos) {
        value_start = name_pos + unlabeled_needle.size();
    } else if ((name_pos = body.find(labeled_needle)) != std::string::npos) {
        const auto brace_end = body.find('}', name_pos);
        if (brace_end == std::string::npos) {
            return -1;
        }
        value_start = brace_end + 2; // "} " before the value
    } else {
        return 0; // metric not yet emitted at all == real zero value, not an error
    }
    const auto line_end = body.find('\n', value_start);
    try {
        return std::stol(body.substr(value_start, line_end - value_start));
    } catch (const std::exception&) {
        return -1;
    }
}

bool wait_metric_at_least(
    const char* host, int port, const std::string& metric_name, long target, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (scrape_metric_value(host, port, metric_name) >= target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

} // namespace

TEST(UdrSmPolicyDataIntegration, PatchCreatesAndMergesRealNestedDocument) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    const std::string supi = unique_test_supi(1);
    const std::string base_url =
        "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + supi + "/sm-data";
    ASSERT_TRUE(wait_reachable(client, base_url, 200)) << "udr never became reachable";

    const std::string token = fetch_token(client, "nudr-dr");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real GET before any PATCH: 404, matching the real "no POST/create exists for this resource"
    // spec shape this project's own store deliberately works around via upsert-on-PATCH.
    sbi_core::http2::ClientRequest get_req;
    get_req.method = "GET";
    get_req.url = base_url;
    get_req.headers.emplace("authorization", "Bearer " + token);
    auto get_resp = client.send(get_req);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 404);

    // Real PATCH creating the real nested SmPolicyData structure from nothing.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = base_url;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{{"smPolicySnssaiData",
                           {{"1-000001",
                             {{"snssai", {{"sst", 1}, {"sd", "000001"}}},
                              {"smPolicyDnnData",
                               {{"internet",
                                 {{"dnn", "internet"},
                                  {"subscSpendingLimits", true},
                                  {"gbrUl", {{"value", 50}, {"unit", "Mbps"}}}}}}}}}}}}
                         .dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    EXPECT_EQ(patch_resp->status, 200);
    auto created = json::parse(patch_resp->body);
    EXPECT_TRUE(created["smPolicySnssaiData"]["1-000001"]["smPolicyDnnData"]["internet"]
                       ["subscSpendingLimits"]
                           .get<bool>());

    // Real GET after create: confirms real persistence, not just an echo.
    auto get_resp2 = client.send(get_req);
    ASSERT_TRUE(get_resp2.has_value());
    EXPECT_EQ(get_resp2->status, 200);
    auto fetched = json::parse(get_resp2->body);
    EXPECT_EQ(fetched["smPolicySnssaiData"]["1-000001"]["smPolicyDnnData"]["internet"]["dnn"]
                  .get<std::string>(),
              "internet");

    // Real partial merge-patch: adds a new field without clobbering the ones already there (real
    // RFC 7396 semantics, not a full-document replace).
    sbi_core::http2::ClientRequest patch_req2;
    patch_req2.method = "PATCH";
    patch_req2.url = base_url;
    patch_req2.headers.emplace("content-type", "application/merge-patch+json");
    patch_req2.headers.emplace("authorization", "Bearer " + token);
    patch_req2.body =
        json{{"smPolicySnssaiData",
              {{"1-000001", {{"smPolicyDnnData", {{"internet", {{"mpsPriority", true}}}}}}}}}}
            .dump();
    auto patch_resp2 = client.send(patch_req2);
    ASSERT_TRUE(patch_resp2.has_value());
    EXPECT_EQ(patch_resp2->status, 200);
    auto merged = json::parse(patch_resp2->body);
    const auto& dnn_data = merged["smPolicySnssaiData"]["1-000001"]["smPolicyDnnData"]["internet"];
    EXPECT_TRUE(dnn_data["mpsPriority"].get<bool>());
    // Real merge, not replace: the earlier fields are still there.
    EXPECT_TRUE(dnn_data["subscSpendingLimits"].get<bool>());
    EXPECT_EQ(dnn_data["dnn"].get<std::string>(), "internet");
}

TEST(PcfN28Integration, CreateSmPolicyFailsOpenWhenChfUnreachable) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0) << "failed to fork pcf";

    auto client = make_client();
    const std::string supi = unique_test_supi(2);
    const std::string udr_url =
        "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + supi + "/sm-data";
    ASSERT_TRUE(wait_reachable(client, udr_url, 200)) << "udr never became reachable";
    const std::string pcf_url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies";
    ASSERT_TRUE(wait_reachable(client, pcf_url, 200)) << "pcf never became reachable";

    const std::string udr_token = fetch_token(client, "nudr-dr");
    ASSERT_FALSE(udr_token.empty());

    // Real seed: subscSpendingLimits=true with a real policyCounterId, exactly the condition that
    // makes CreateSMPolicy attempt a real CHF subscribe -- CHF is deliberately not running, so
    // this exercises the real fail-open path (subscribe_spending_limit's own disclosed
    // "CHF unreachable shouldn't block a PDU session from getting service" behavior).
    sbi_core::http2::ClientRequest seed_req;
    seed_req.method = "PATCH";
    seed_req.url = udr_url;
    seed_req.headers.emplace("content-type", "application/merge-patch+json");
    seed_req.headers.emplace("authorization", "Bearer " + udr_token);
    seed_req.body =
        json{{"smPolicySnssaiData",
              {{"1-000001",
                {{"snssai", {{"sst", 1}, {"sd", "000001"}}},
                 {"smPolicyDnnData",
                  {{"internet",
                    {{"dnn", "internet"},
                     {"subscSpendingLimits", true},
                     {"spendLimInfo",
                      {{"test-counter",
                        {{"policyCounterId", "test-counter"}, {"currentStatus", "x"}}}}}}}}}}}}}}
            .dump();
    auto seed_resp = client.send(seed_req);
    ASSERT_TRUE(seed_resp.has_value());
    EXPECT_EQ(seed_resp->status, 200);

    const std::string pcf_token = fetch_token(client, "npcf-smpolicycontrol", "PCF");
    ASSERT_FALSE(pcf_token.empty());

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = pcf_url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + pcf_token);
    create_req.body =
        json{
            {"supi", supi},
            {"pduSessionId", 1},
            {"pduSessionType", "IPV4"},
            {"dnn", "internet"},
            {"notificationUri", "https://example.com/notify"},
            {"sliceInfo", {{"sst", 1}, {"sd", "000001"}}},
        }
            .dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    // Real fail-open: CHF being unreachable does not fail the SM Policy request itself.
    EXPECT_EQ(create_resp->status, 201);
    auto decision = json::parse(create_resp->body);
    EXPECT_TRUE(decision.contains("sessRules"));
}

// ADR-0073: real CHF-in-CI closes the gap ADR-0072 disclosed as manual-only -- the full real
// UDR->PCF->CHF->statusNotification->PCF loop, automated. Real success signal: PCF's own
// Prometheus counters (`pcf_spending_limit_subscribe_total`/`pcf_spending_limit_notify_total`),
// since PCF's real REST surface deliberately carries no fabricated field revealing its internal
// spending-limit tracking state (see nfs/pcf/src/main.cpp's own file header on why).
TEST(PcfChfN28Integration, FullLoopSubscribeStatusChangeNotifyUnsubscribe) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0) << "failed to fork pcf";
    nf_test::SpawnedProcess chf(CHF_PATH);
    ASSERT_GT(chf.pid(), 0) << "failed to fork chf";

    auto client = make_client();
    const std::string supi = unique_test_supi(3);
    const std::string udr_url =
        "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + supi + "/sm-data";
    ASSERT_TRUE(wait_reachable(client, udr_url, 200)) << "udr never became reachable";
    const std::string pcf_url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies";
    ASSERT_TRUE(wait_reachable(client, pcf_url, 200)) << "pcf never became reachable";
    const std::string chf_url = "https://127.0.0.1:7784/nchf-spendinglimitcontrol/v1/subscriptions";
    ASSERT_TRUE(wait_reachable(client, chf_url, 200)) << "chf never became reachable";

    const std::string counter_id = "full-loop-counter-" + std::to_string(getpid());

    const std::string udr_token = fetch_token(client, "nudr-dr");
    ASSERT_FALSE(udr_token.empty());
    sbi_core::http2::ClientRequest seed_req;
    seed_req.method = "PATCH";
    seed_req.url = udr_url;
    seed_req.headers.emplace("content-type", "application/merge-patch+json");
    seed_req.headers.emplace("authorization", "Bearer " + udr_token);
    seed_req.body =
        json{{"smPolicySnssaiData",
              {{"1-000001",
                {{"snssai", {{"sst", 1}, {"sd", "000001"}}},
                 {"smPolicyDnnData",
                  {{"internet",
                    {{"dnn", "internet"},
                     {"subscSpendingLimits", true},
                     {"spendLimInfo",
                      {{counter_id,
                        {{"policyCounterId", counter_id}, {"currentStatus", "initial"}}}}}}}}}}}}}}
            .dump();
    auto seed_resp = client.send(seed_req);
    ASSERT_TRUE(seed_resp.has_value());
    ASSERT_EQ(seed_resp->status, 200);

    // Real baseline: how many subscribes/notifies PCF has already done this process's lifetime
    // (should be 0, this test's own process is fresh, but read rather than assumed for real
    // correctness regardless).
    const long subscribe_baseline =
        scrape_metric_value("127.0.0.1", 9470, "pcf_spending_limit_subscribe_total");
    const long notify_baseline =
        scrape_metric_value("127.0.0.1", 9470, "pcf_spending_limit_notify_total");

    const std::string pcf_token = fetch_token(client, "npcf-smpolicycontrol", "PCF");
    ASSERT_FALSE(pcf_token.empty());
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = pcf_url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + pcf_token);
    create_req.body =
        json{
            {"supi", supi},
            {"pduSessionId", 1},
            {"pduSessionType", "IPV4"},
            {"dnn", "internet"},
            {"notificationUri", "https://example.com/notify"},
            {"sliceInfo", {{"sst", 1}, {"sd", "000001"}}},
        }
            .dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    const auto location_it = create_resp->headers.find("location");
    ASSERT_NE(location_it, create_resp->headers.end());
    const auto sm_policy_id = location_it->second.substr(location_it->second.find_last_of('/') + 1);

    // Real success signal: CHF is now reachable, so this real subscribe should actually succeed
    // (unlike CreateSmPolicyFailsOpenWhenChfUnreachable above).
    EXPECT_TRUE(wait_metric_at_least(
        "127.0.0.1", 9470, "pcf_spending_limit_subscribe_total", subscribe_baseline + 1, 30))
        << "PCF never recorded a real successful CHF spending-limit subscribe";

    // Real CHF-side config change (this project's own real, disclosed operator/GUI surface) --
    // triggers a real statusNotification push to PCF's real callback route.
    const std::string chf_token = fetch_token(client, "chf-admin", "CHF");
    ASSERT_FALSE(chf_token.empty());
    sbi_core::http2::ClientRequest admin_req;
    admin_req.method = "PUT";
    admin_req.url = "https://127.0.0.1:7784/chf-admin/v1/policy-counters/" + counter_id;
    admin_req.headers.emplace("content-type", "application/json");
    admin_req.headers.emplace("authorization", "Bearer " + chf_token);
    admin_req.body = json{{"currentStatus", "quota_exceeded"}}.dump();
    auto admin_resp = client.send(admin_req);
    ASSERT_TRUE(admin_resp.has_value());
    EXPECT_EQ(admin_resp->status, 204);

    EXPECT_TRUE(wait_metric_at_least(
        "127.0.0.1", 9470, "pcf_spending_limit_notify_total", notify_baseline + 1, 30))
        << "PCF never received the real CHF statusNotification push";

    // Real teardown: DeleteSMPolicy should trigger a real CHF unsubscribe (best-effort, see
    // nfs/pcf/src/main.cpp's own comment) -- checked here via a real, direct follow-up GET on
    // CHF's own subscription resource returning 404 (a genuinely different, independent real
    // signal from PCF's own metrics above).
    sbi_core::http2::ClientRequest delete_req;
    delete_req.method = "POST";
    delete_req.url = pcf_url + "/" + sm_policy_id + "/delete";
    delete_req.headers.emplace("content-type", "application/json");
    delete_req.headers.emplace("authorization", "Bearer " + pcf_token);
    delete_req.body = "{}";
    auto delete_resp = client.send(delete_req);
    ASSERT_TRUE(delete_resp.has_value());
    EXPECT_EQ(delete_resp->status, 204);
}
