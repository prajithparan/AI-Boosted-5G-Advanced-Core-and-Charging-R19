// Nnwdaf_MLModelTraining subscription CRUD (ADR-0471, increment 1) --
// TS29520_Nnwdaf_MLModelTraining.yaml (v1.1.0), TS 23.288 7.10.2 / 7.10.3. An NWDAF in role mtlf
// behind a real NRF and a real Valkey.
//
// Covers: POST (201 + Location + failEventReports for an event it does not train; 500 when none is
// trainable; 400 for a missing notifCorreId), PUT (200 / 404), PATCH merge-patch (200, a member
// outside NwdafMLModelTrainSubscPatch is 400), DELETE (204 / 404), UnsubscribeInfo (204, 400
// without termCause, 404), and the model notification + immReport (increment 2: a model record
// is SEEDED in Valkey, there is no ADRF or trainer in this lab). It does NOT cover delayEventNotif,
// statusReport, termTrainReq or any federated-learning behaviour: none exists yet (ADR-0471).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <sw/redis++/redis++.h>
#include <thread>
#include <vector>

#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr const char* kNrf = "https://127.0.0.1:7777";
constexpr const char* kMtlf = "https://127.0.0.1:7797";
constexpr const char* kRoot = "/nnwdaf-mlmodeltraining/v1";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

bool wait_up(sbi_core::http2::Client& c, const std::string& url, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        if (auto r = c.send(req); r && r->status != 0) {
            return true;
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

std::string token(sbi_core::http2::Client& c) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string(kNrf) + "/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=nnwdaf-mlmodeltraining"
               "&targetNfType=NWDAF";
    auto resp = c.send(req);
    return (resp && resp->status == 200)
               ? json::parse(resp->body).at("access_token").get<std::string>()
               : "";
}

json subscription(const json& events = json::array({json{{"mLEvent", "NF_LOAD"},
                                                         {"mLEventFilter", json::object()}}})) {
    return json{{"mLEventSubscs", events},
                {"notifUri", "https://127.0.0.1:19994/notify"},
                {"notifCorreId", "corr-1"}};
}

constexpr int kReceiverPort = 19994;

// Training subscriptions live in the shared lab Valkey and outlive a test process. A leftover that
// points at this test's receiver sends its own copy of every model notification and breaks the
// counts below, so the test removes exactly those records (matched on its receiver port) before
// it starts and when it ends. Records of any other consumer are left alone.
void purge_subscriptions_to_the_test_receiver(sw::redis::Redis& redis) {
    std::vector<std::string> ids;
    redis.smembers("nwdaf:mltrainsub:subs", std::back_inserter(ids));
    const std::string mine = "127.0.0.1:" + std::to_string(kReceiverPort);
    for (const auto& id : ids) {
        const auto raw = redis.get("nwdaf:mltrainsub:sub:" + id);
        if (raw && raw->find(mine) != std::string::npos) {
            redis.del("nwdaf:mltrainsub:sub:" + id);
            redis.srem("nwdaf:mltrainsub:subs", id);
        }
    }
}

// An HTTP/2 + mTLS endpoint standing in for the consumer's notifUri.
class Receiver {
public:
    Receiver()
        : server_(ioc_,
                  "127.0.0.1",
                  kReceiverPort,
                  sbi_core::http2::TlsConfig{.cert_path = CERTS_DIR "/hello-nf/cert.pem",
                                             .key_path = CERTS_DIR "/hello-nf/key.pem",
                                             .ca_path = CERTS_DIR "/ca/ca.crt"}) {
        server_.add_route("POST", "/notify", [this](const sbi_core::http2::Request& req) {
            std::string callback;
            if (const auto it = req.headers.find("3gpp-sbi-callback"); it != req.headers.end()) {
                callback = it->second;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                notes_.push_back(Note{json::parse(req.body), callback});
            }
            cv_.notify_all();
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
        server_.start();
        thread_ = std::thread([this] { ioc_.run(); });
    }
    ~Receiver() {
        ioc_.stop();
        thread_.join();
    }
    struct Note {
        json body;
        std::string callback;
    };
    bool wait_for_count(std::size_t n, std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [&] { return notes_.size() >= n; });
    }
    std::vector<Note> notes() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return notes_;
    }

private:
    boost::asio::io_context ioc_;
    sbi_core::http2::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Note> notes_;
};

} // namespace

TEST(NwdafMlTrainingSub, SubscriptionLifecycle) {
    const std::string redis_url =
        std::getenv("NWDAF_REDIS_URL") ? std::getenv("NWDAF_REDIS_URL") : "tcp://127.0.0.1:6379";
    try {
        sw::redis::Redis(redis_url).ping();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "no Valkey at " << redis_url << " (" << e.what()
                     << ") -- skipped, not passed";
    }
    auto c = make_client();
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_TRUE(wait_up(c, std::string(kNrf) + "/nnrf-nfm/v1/nf-instances", 30s));
    setenv("NWDAF_ROLE", "mtlf", 1);
    setenv("NWDAF_PORT", "7797", 1);
    setenv("NWDAF_METRICS_BIND_ADDRESS", "0.0.0.0:9486", 1);
    setenv("NWDAF_SELF_BASE_URL", kMtlf, 1);
    nf_test::SpawnedProcess mtlf(NWDAF_PATH);
    for (const char* k :
         {"NWDAF_ROLE", "NWDAF_PORT", "NWDAF_METRICS_BIND_ADDRESS", "NWDAF_SELF_BASE_URL"}) {
        unsetenv(k);
    }
    ASSERT_TRUE(wait_up(c, std::string(kMtlf) + kRoot + "/subscriptions", 30s))
        << "MTLF never came up";

    const auto tok = token(c);
    ASSERT_FALSE(tok.empty());
    const auto call = [&](const std::string& method,
                          const std::string& path,
                          const json* body,
                          const char* content_type = "application/json") {
        sbi_core::http2::ClientRequest req;
        req.method = method;
        req.url = std::string(kMtlf) + path;
        req.headers.emplace("authorization", "Bearer " + tok);
        if (body != nullptr) {
            req.headers.emplace("content-type", content_type);
            req.body = body->dump();
        }
        return c.send(req);
    };

    // POST: 400 without notifCorreId, 500 when no event is trainable, 201 + Location otherwise.
    json missing = subscription();
    missing.erase("notifCorreId");
    auto r = call("POST", std::string(kRoot) + "/subscriptions", &missing);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 400) << r->body;

    const json untrainable = subscription(
        json::array({json{{"mLEvent", "UE_MOBILITY"}, {"mLEventFilter", json::object()}}}));
    r = call("POST", std::string(kRoot) + "/subscriptions", &untrainable);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 500) << r->body;
    // TS 29.520 table 5.5.7.3-1: the Training service's own cause, not Provision's.
    EXPECT_EQ(json::parse(r->body).value("cause", ""),
              "UNAVAILABLE_ML_MODEL_TRAINING_FOR_ALLEVENTS")
        << r->body;

    const json mixed = subscription(
        json::array({json{{"mLEvent", "NF_LOAD"}, {"mLEventFilter", json::object()}},
                     json{{"mLEvent", "UE_MOBILITY"}, {"mLEventFilter", json::object()}}}));
    r = call("POST", std::string(kRoot) + "/subscriptions", &mixed);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto fails = json::parse(r->body).at("failEventReports");
    ASSERT_EQ(fails.size(), 1U) << r->body;
    // FailureEventInfoForMLModelTrain: both members are required by the YAML.
    EXPECT_EQ(fails[0].value("mLTrainEvent", ""), "UE_MOBILITY") << r->body;
    EXPECT_EQ(fails[0].value("failureCodeTrain", ""), "UNAVAILABLE_ML_MODEL_TRAIN") << r->body;
    const auto loc = r->headers.find("location");
    ASSERT_NE(loc, r->headers.end());
    const std::string item = loc->second.substr(loc->second.find(kRoot));

    // PUT replaces; a stale failEventReports from the old state does not survive.
    const json replacement = subscription();
    r = call("PUT", item, &replacement);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 200) << r->body;
    EXPECT_FALSE(json::parse(r->body).contains("failEventReports")) << r->body;
    r = call("PUT", std::string(kRoot) + "/subscriptions/does-not-exist", &replacement);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404) << r->body;

    // PATCH (RFC 7396): changes notifUri and adds roundInd; a member outside the patch schema is
    // 400.
    const json patch = {{"notifUri", "https://127.0.0.1:19994/other"}, {"roundInd", 2}};
    r = call("PATCH", item, &patch, "application/merge-patch+json");
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body).at("notifUri"), "https://127.0.0.1:19994/other");
    EXPECT_EQ(json::parse(r->body).at("roundInd"), 2);
    EXPECT_EQ(json::parse(r->body).at("notifCorreId"), "corr-1");
    const json bad_patch = {{"notifCorreId", "changed"}};
    r = call("PATCH", item, &bad_patch, "application/merge-patch+json");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400) << r->body;

    // UnsubscribeInfo: 400 without termCause; 204 removes; then 404.
    const json no_cause = json::object();
    r = call("POST", item + "/unsubscribe-info", &no_cause);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400) << r->body;
    // termCause is an FLServerTermCause (FL_CLI_UNSELECTED, FL_SUSPENDED, FL_FINISHED, OTHER).
    const json with_cause = {{"termCause", "FL_FINISHED"}};
    r = call("POST", item + "/unsubscribe-info", &with_cause);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = call("POST", item + "/unsubscribe-info", &with_cause);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404) << r->body;

    // DELETE: 204 then 404.
    r = call("POST", std::string(kRoot) + "/subscriptions", &replacement);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto l2 = r->headers.find("location");
    ASSERT_NE(l2, r->headers.end());
    const std::string item2 = l2->second.substr(l2->second.find(kRoot));
    r = call("DELETE", item2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 204);
    r = call("DELETE", item2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);
}

TEST(NwdafMlTrainingSub, ModelNotificationAndImmReportFromASeededModel) {
    const std::string redis_url =
        std::getenv("NWDAF_REDIS_URL") ? std::getenv("NWDAF_REDIS_URL") : "tcp://127.0.0.1:6379";
    std::unique_ptr<sw::redis::Redis> redis;
    try {
        redis = std::make_unique<sw::redis::Redis>(redis_url);
        redis->ping();
    } catch (const std::exception& e) {
        GTEST_SKIP() << "no Valkey at " << redis_url << " (" << e.what()
                     << ") -- skipped, not passed";
    }
    const auto seed = [&](int version) {
        redis->set("nwdaf:mlmodel:NF_LOAD",
                   json{{"version", version},
                        {"modelUniqueId", 4242},
                        {"fileUrl", "https://adrf.example/models/4242"},
                        {"adrfId", "adrf-seeded"},
                        {"storeTransId", "trans-seeded"},
                        {"accuracyPct", 91},
                        {"dataSource", "seeded"},
                        {"trainedAt", "2026-10-08T00:00:00Z"}}
                       .dump());
    };
    // A model must be absent when the subscription is created (immReport empty), then appear.
    redis->del("nwdaf:mlmodel:NF_LOAD");
    purge_subscriptions_to_the_test_receiver(*redis);

    Receiver receiver;
    auto c = make_client();
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_TRUE(wait_up(c, std::string(kNrf) + "/nnrf-nfm/v1/nf-instances", 30s));
    setenv("NWDAF_ROLE", "mtlf", 1);
    setenv("NWDAF_PORT", "7797", 1);
    setenv("NWDAF_METRICS_BIND_ADDRESS", "0.0.0.0:9486", 1);
    setenv("NWDAF_SELF_BASE_URL", kMtlf, 1);
    setenv("NWDAF_MTLF_CHECK_INTERVAL_SECONDS", "2", 1);
    nf_test::SpawnedProcess mtlf(NWDAF_PATH);
    for (const char* k : {"NWDAF_ROLE",
                          "NWDAF_PORT",
                          "NWDAF_METRICS_BIND_ADDRESS",
                          "NWDAF_SELF_BASE_URL",
                          "NWDAF_MTLF_CHECK_INTERVAL_SECONDS"}) {
        unsetenv(k);
    }
    ASSERT_TRUE(wait_up(c, std::string(kMtlf) + kRoot + "/subscriptions", 30s));
    const auto tok = token(c);
    ASSERT_FALSE(tok.empty());
    const auto post = [&](const json& body) {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = std::string(kMtlf) + kRoot + "/subscriptions";
        req.headers.emplace("authorization", "Bearer " + tok);
        req.headers.emplace("content-type", "application/json");
        req.body = body.dump();
        return c.send(req);
    };

    // immReport is asked for but no model exists yet: none is returned.
    json sub = subscription();
    sub["eventReq"] = json{{"immRep", true}};
    auto r = post(sub);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    EXPECT_FALSE(json::parse(r->body).contains("immReport")) << r->body;

    // A model appears: the MTLF loop notifies mLModelInfos + notifCorreId, not an update.
    seed(1);
    ASSERT_TRUE(receiver.wait_for_count(1, 30s)) << "no model notification";
    {
        const auto note = receiver.notes().at(0);
        EXPECT_EQ(note.callback, "Nnwdaf_MLModelTraining_myNotification");
        EXPECT_EQ(note.body.at("notifCorreId"), "corr-1");
        const auto& info = note.body.at("mLModelInfos").at(0);
        EXPECT_EQ(info.at("event"), "NF_LOAD");
        EXPECT_EQ(info.at("modelUniqueId"), 4242);
        EXPECT_EQ(info.at("modelUpdateInd"), false);
    }
    // Re-trained (version bump): a second notification flagged as an update.
    seed(2);
    ASSERT_TRUE(receiver.wait_for_count(2, 30s)) << "no re-train notification";
    {
        const auto all = receiver.notes();
        std::string dump;
        for (const auto& n : all) {
            dump += n.body.dump() + "\n";
        }
        EXPECT_EQ(all.at(1).body.at("mLModelInfos").at(0).at("modelUpdateInd"), true)
            << "notifications received so far:\n"
            << dump;
    }

    // A new subscription with immRep now gets the report in the 201 body.
    r = post(sub);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto body = json::parse(r->body);
    ASSERT_TRUE(body.contains("immReport")) << r->body;
    EXPECT_EQ(body.at("immReport").at("notifCorreId"), "corr-1");
    EXPECT_EQ(body.at("immReport").at("mLModelInfos").at(0).at("modelUniqueId"), 4242);

    redis->del("nwdaf:mlmodel:NF_LOAD");
    purge_subscriptions_to_the_test_receiver(*redis);
}
