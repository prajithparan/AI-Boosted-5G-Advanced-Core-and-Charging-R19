// ADRF end to end (ADR-0367): TS 29.575 against a real NRF, Valkey, Apache Doris and PostgreSQL --
// and, for the collection paths, a real NWDAF and the DCCF + MFAF + Kafka chain.
//
//   * StorageRequest / RetrievalRequest / Delete (4.2.2.2, 4.2.2.5, 4.2.2.9): a record round-trips
//     through Doris by storeTransId and by dataSetId; the operator's lifetime policy bounds what
//     the consumer asked for; a record breaking the oneOf is refused.
//   * RetrievalSubscribe / RetrievalNotify (4.2.2.6, 4.2.2.8.2): what is already stored inside
//     the timePeriod is notified on subscription, what arrives later is notified on arrival,
//     consTrigNotif turns that into fetch instructions redeemed by RetrievalRequest; the
//     3gpp-Sbi-Callback header names the callback.
//   * Deletion alerts (4.2.2.8.3): a record with delNotifUri and a short lifetime produces an
//     alert before deletion; answering retrievalInd defers the deletion by the grace period.
//   * StorageSubscriptionRequest towards an NWDAF (4.2.2.3, targetNfId resolved at the NRF):
//     the ADRF subscribes NF_LOAD itself and stores what the NWDAF notifies; removal (4.2.2.4)
//     stops that.
//   * The same through a DCCF (needs Kafka for the MFAF): an NRF NFStatus data subscription,
//     stored when an NF appears.
//   * MLModelManagement (4.3): inline model stored and served at the address the ADRF hands out,
//     the allowed-consumer list enforced (403 RETRIEVAL_ML_MODEL_NOT_ALLOWED), download by
//     mlFileAddr from a consumer-side server, all-failed 404, per-model delete results.
//
// Skipped, not passed, without Doris (the NWDAF tests' feature-store env is reused) or, for the
// DCCF path, without a Kafka broker and under ThreadSanitizer (ADR-0364 CI note 3).
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <librdkafka/rdkafkacpp.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spawn_guard.hpp"

#include <gtest/gtest.h>

#if defined(__SANITIZE_THREAD__)
#define ADRF_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ADRF_TEST_TSAN 1
#endif
#endif

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr const char* kNrf = "https://127.0.0.1:7777";
constexpr const char* kAdrf = "https://127.0.0.1:7804";
constexpr const char* kDccf = "https://127.0.0.1:7803";
constexpr const char* kDm = "/nadrf-datamanagement/v1";
constexpr const char* kMl = "/nadrf-mlmodelmanagement/v1";
constexpr int kReceiverPort = 19993;

std::string brokers() {
    if (const char* env = std::getenv("MFAF_EVENT_BUS_BROKERS")) {
        return env;
    }
    return "127.0.0.1:9092";
}

bool broker_reachable(const std::string& b) {
    std::string err;
    std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    conf->set("bootstrap.servers", b, err);
    std::unique_ptr<RdKafka::Producer> p(RdKafka::Producer::create(conf.get(), err));
    if (!p) {
        return false;
    }
    RdKafka::Metadata* md = nullptr;
    const auto rc = p->metadata(true, nullptr, &md, 3000);
    delete md;
    return rc == RdKafka::ERR_NO_ERROR;
}

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

std::string token_as(sbi_core::http2::Client& c,
                     const std::string& nf_instance_id,
                     const std::string& scope,
                     const std::string& target_nf) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string(kNrf) + "/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=" + nf_instance_id + "&scope=" + scope +
               "&targetNfType=" + target_nf;
    auto resp = c.send(req);
    if (!resp || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

sbi_core::http2::ClientRequest request(const std::string& method, const std::string& url) {
    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    return req;
}

sbi_core::http2::ClientRequest
request(const std::string& method, const std::string& url, const json& body) {
    auto req = request(method, url);
    req.headers.emplace("content-type", "application/json");
    req.body = body.dump();
    return req;
}

std::string now_plus(std::chrono::seconds delta) {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + delta);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

json window(std::chrono::seconds before, std::chrono::seconds after) {
    return json{{"startTime", now_plus(-before)}, {"stopTime", now_plus(after)}};
}

// The consumer side: notification endpoints, a deletion-alert endpoint that asks for a grace
// period, and a model file for the download path.
class Receiver {
public:
    Receiver()
        : server_(ioc_,
                  "127.0.0.1",
                  kReceiverPort,
                  sbi_core::http2::TlsConfig{.cert_path = CERTS_DIR "/hello-nf/cert.pem",
                                             .key_path = CERTS_DIR "/hello-nf/key.pem",
                                             .ca_path = CERTS_DIR "/ca/ca.crt"}) {
        for (const char* path : {"/notify", "/notify-fetch", "/alert"}) {
            server_.add_route("POST", path, [this, path](const sbi_core::http2::Request& req) {
                std::string callback;
                if (const auto it = req.headers.find("3gpp-sbi-callback");
                    it != req.headers.end()) {
                    callback = it->second;
                }
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    notes_.push_back(Note{path, json::parse(req.body), callback});
                }
                cv_.notify_all();
                sbi_core::http2::Response resp;
                if (std::string(path) == "/alert") {
                    resp.status = 200;
                    resp.headers.emplace("content-type", "application/json");
                    resp.body = json{{"retrievalInd", true}}.dump();
                } else {
                    resp.status = 204;
                }
                return resp;
            });
        }
        server_.add_route("GET", "/model.bin", [](const sbi_core::http2::Request&) {
            sbi_core::http2::Response resp;
            resp.status = 200;
            resp.headers.emplace("content-type", "application/octet-stream");
            resp.body = std::string("\x00\x01\x02ONNX-model-bytes\xff", 20);
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
        std::string path;
        json body;
        std::string callback;
    };
    static std::string url(const char* path) {
        return "https://127.0.0.1:" + std::to_string(kReceiverPort) + path;
    }
    bool wait_for(const char* path, std::size_t at_least, std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [&] { return count_locked(path) >= at_least; });
    }
    std::vector<Note> notes(const char* path) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Note> out;
        for (const auto& n : notes_) {
            if (n.path == path) {
                out.push_back(n);
            }
        }
        return out;
    }

private:
    std::size_t count_locked(const char* path) {
        std::size_t n = 0;
        for (const auto& note : notes_) {
            n += note.path == path;
        }
        return n;
    }
    boost::asio::io_context ioc_;
    sbi_core::http2::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Note> notes_;
};

json analytics_record(const std::string& marker) {
    // anaSub: the NnwdafEventsSubscription the notifications belong to; anaNotifications: what
    // the NWDAF sent (TS 29.520 NnwdafEventsSubscriptionNotification). The marker rides in the
    // subscription's nfInstanceIds filter, which the spec fingerprint keeps (notification-target
    // fields are stripped), so every run of this test sees only its own records and retrieval
    // subscriptions even on a lab Valkey/Doris that earlier runs left state in.
    return json{
        {"anaSub",
         json::array({json{
             {"eventSubscriptions",
              json::array({json{{"event", "NF_LOAD"}, {"nfInstanceIds", json::array({marker})}}})},
             {"notificationURI", "https://consumer.invalid/" + marker},
             {"notifCorrId", marker}}})},
        {"anaNotifications",
         json::array({json{{"subscriptionId", "nwdaf-sub-" + marker},
                           {"eventNotifications",
                            json::array({json{{"event", "NF_LOAD"},
                                              {"nfLoadLevelInfos",
                                               json::array({json{{"nfType", "AMF"},
                                                                 {"nfCpuUsage", 7}}})}}})}}})}};
}

json nrf_data_record(const std::string& marker) {
    return json{
        {"dataSub",
         json::array({json{{"nrfDataSub",
                            json{{"nfStatusNotificationUri", "https://consumer.invalid/" + marker},
                                 {"subscrCond", json{{"nfType", "AMF"}}}}}}})},
        {"dataNotif",
         json{
             {"nrfEventNotifs",
              json::array({json{{"event", "NF_REGISTERED"},
                                {"nfInstanceUri", "https://nrf.invalid/nf-instances/" + marker}}})},
             {"timeStamp", now_plus(0s)}}}};
}

struct Lab {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess adrf;
};

Lab spawn_lab() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    // Short reaper cadence so the deletion-alert test finishes in seconds; the rest is the
    // shipped config/adrf.json.
    setenv("ADRF_ALERT_LEAD_SECONDS", "1", 1);
    setenv("ADRF_ALERT_GRACE_SECONDS", "4", 1);
    setenv("ADRF_REAPER_INTERVAL_SECONDS", "1", 1);
    setenv("ADRF_RETRIEVAL_SWEEP_INTERVAL_SECONDS", "1", 1);
    nf_test::SpawnedProcess adrf(ADRF_PATH);
    unsetenv("ADRF_ALERT_LEAD_SECONDS");
    unsetenv("ADRF_ALERT_GRACE_SECONDS");
    unsetenv("ADRF_REAPER_INTERVAL_SECONDS");
    unsetenv("ADRF_RETRIEVAL_SWEEP_INTERVAL_SECONDS");
    return Lab{std::move(nrf), std::move(adrf)};
}

// Skip, not fail, only when Doris itself is absent (the ADRF answers 500 "data store
// unreachable"); a Doris that is up but missing the adrf_data schema is a real failure and the
// assertions that follow will report it.
bool data_store_available() {
    auto c = make_client();
    auto r = c.send(
        request("GET", std::string(kAdrf) + kDm + "/data-store-records?store-trans-id=probe"));
    return !(r.has_value() && r->status == 500 &&
             r->body.find("data store unreachable") != std::string::npos);
}

} // namespace

TEST(Adrf, StoresRetrievesAndDeletesRecords) {
    auto lab = spawn_lab();
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + kDm + "/data-store-records", 30s))
        << "ADRF never came up";
    if (!data_store_available()) {
        GTEST_SKIP() << "no Apache Doris behind the ADRF -- skipped, not passed";
    }

    // A record breaking the oneOf: dataSub without dataNotif.
    auto r = c.send(request("POST",
                            std::string(kAdrf) + kDm + "/data-store-records",
                            json{{"dataSub", nrf_data_record("x")["dataSub"]}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 400) << r->body;

    // Store with a lifetime beyond the operator maximum: applied, bounded, echoed.
    json rec = nrf_data_record("one");
    rec["storeHandl"] = json{{"lifetime", 99999999999}};
    r = c.send(request("POST", std::string(kAdrf) + kDm + "/data-store-records", rec));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto created = json::parse(r->body);
    EXPECT_EQ(created["storeHandl"]["lifetime"], 2592000); // config max_lifetime_seconds
    EXPECT_EQ(created["suppFeat"], "4");                   // EnhDataMgmt
    const std::string location = r->headers.find("location")->second;
    ASSERT_NE(location.find(std::string(kDm) + "/data-store-records/"), std::string::npos);
    const std::string store_trans_id = location.substr(location.rfind('/') + 1);

    r = c.send(request(
        "GET", std::string(kAdrf) + kDm + "/data-store-records?store-trans-id=" + store_trans_id));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    {
        const auto got = json::parse(r->body);
        ASSERT_TRUE(got.contains("dataNotif")) << got.dump();
        EXPECT_EQ(got["dataNotif"]["nrfEventNotifs"][0]["nfInstanceUri"],
                  "https://nrf.invalid/nf-instances/one");
        EXPECT_EQ(got["dataSub"].size(), 1U);
    }

    // Two analytics records under one data set; retrieved merged, deleted by data set + window.
    for (const char* m :
         {"00000000-0000-4000-8000-00000000d5a1", "00000000-0000-4000-8000-00000000d5b2"}) {
        json ar = analytics_record(m);
        ar["dataSetTag"] = json{{"dataSetId", "set-42"}, {"dataSetDesc", "test set"}};
        r = c.send(request("POST", std::string(kAdrf) + kDm + "/data-store-records", ar));
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 201) << r->body;
    }
    r = c.send(request("GET", std::string(kAdrf) + kDm + "/data-store-records?data-set-id=set-42"));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    {
        const auto got = json::parse(r->body);
        ASSERT_TRUE(got.contains("anaNotifications")) << got.dump();
        EXPECT_EQ(got["anaNotifications"].size(), 2U);
        EXPECT_EQ(got["anaSub"].size(), 2U);
        EXPECT_EQ(got["dataSetTag"]["dataSetId"], "set-42");
    }
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/remove-stored-data-analytics",
                       json{{"dataSetId", "set-42"}, {"timePeriod", window(3600s, 3600s)}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(request("GET", std::string(kAdrf) + kDm + "/data-store-records?data-set-id=set-42"));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;

    // Delete by storeTransId, then it is gone.
    r = c.send(request("DELETE", std::string(kAdrf) + location));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(request("DELETE", std::string(kAdrf) + location));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;
    r = c.send(request(
        "GET", std::string(kAdrf) + kDm + "/data-store-records?store-trans-id=" + store_trans_id));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
}

TEST(Adrf, RetrievalSubscriptionsNotifyStoredAndFutureRecords) {
    Receiver receiver;
    auto lab = spawn_lab();
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + kDm + "/data-store-records", 30s))
        << "ADRF never came up";
    if (!data_store_available()) {
        GTEST_SKIP() << "no Apache Doris behind the ADRF -- skipped, not passed";
    }
    // A run-unique NfInstanceId (UUID shape, TS 29.571) for the subscription's nfInstanceIds.
    char marker[64];
    std::snprintf(marker,
                  sizeof marker,
                  "00000000-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(std::time(nullptr)));
    const json spec = analytics_record(marker)["anaSub"][0];

    // Already stored, inside the window: notified on subscription.
    auto r = c.send(request(
        "POST", std::string(kAdrf) + kDm + "/data-store-records", analytics_record(marker)));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;

    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/data-retrieval-subscriptions",
                       json{{"notifCorrId", "rs-corr"},
                            {"anaSub", spec},
                            {"notificationURI", Receiver::url("/notify")},
                            {"timePeriod", window(3600s, 3600s)}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string sub_location = r->headers.find("location")->second;
    ASSERT_TRUE(receiver.wait_for("/notify", 1, 20s)) << "stored records were not notified";
    {
        const auto n = receiver.notes("/notify")[0];
        EXPECT_EQ(n.callback, "Nadrf_DataManagement_adrfDataRetrievalNotification");
        EXPECT_EQ(n.body["notifCorrId"], "rs-corr");
        ASSERT_TRUE(n.body.contains("anaNotifications")) << n.body.dump();
        EXPECT_EQ(n.body["anaNotifications"][0]["subscriptionId"],
                  "nwdaf-sub-" + std::string(marker));
        EXPECT_TRUE(n.body.contains("timeStamp"));
    }

    // Arriving later with the same spec: notified on arrival; a different spec is not.
    r = c.send(request(
        "POST", std::string(kAdrf) + kDm + "/data-store-records", analytics_record(marker)));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    ASSERT_TRUE(receiver.wait_for("/notify", 2, 20s)) << "the later record was not notified";
    json other = analytics_record(marker);
    other["anaSub"][0]["eventSubscriptions"][0]["event"] = "ABNORMAL_BEHAVIOUR";
    r = c.send(request("POST", std::string(kAdrf) + kDm + "/data-store-records", other));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    EXPECT_FALSE(receiver.wait_for("/notify", 3, 3s)) << "a record of another spec was notified";

    // consTrigNotif: fetch instructions instead of content, redeemed by RetrievalRequest.
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/data-retrieval-subscriptions",
                       json{{"notifCorrId", "rs-fetch"},
                            {"anaSub", spec},
                            {"notificationURI", Receiver::url("/notify-fetch")},
                            {"timePeriod", window(3600s, 3600s)},
                            {"consTrigNotif", true}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string fetch_sub_location = r->headers.find("location")->second;
    ASSERT_TRUE(receiver.wait_for("/notify-fetch", 1, 20s)) << "no fetch instruction arrived";
    {
        const auto n = receiver.notes("/notify-fetch")[0];
        ASSERT_TRUE(n.body.contains("fetchInstruct")) << n.body.dump();
        EXPECT_FALSE(n.body.contains("anaNotifications"));
        const auto ids = n.body["fetchInstruct"]["fetchCorrIds"];
        ASSERT_EQ(ids.size(), 2U) << n.body.dump(); // both stored records of this spec
        EXPECT_EQ(n.body["fetchInstruct"]["fetchUri"],
                  std::string(kAdrf) + kDm + "/data-store-records");
        std::string list = ids[0].get<std::string>() + "," + ids[1].get<std::string>();
        r = c.send(request(
            "GET", std::string(kAdrf) + kDm + "/data-store-records?fetch-correlation-ids=" + list));
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 200) << r->body;
        EXPECT_EQ(json::parse(r->body)["anaNotifications"].size(), 2U);
        // Consumed once: the same ids fetch nothing the second time.
        r = c.send(request(
            "GET", std::string(kAdrf) + kDm + "/data-store-records?fetch-correlation-ids=" + list));
        ASSERT_TRUE(r.has_value()) << r.error();
        EXPECT_EQ(r->status, 204) << r->body;
    }

    // Unsubscribe: no more notifications for that spec on this endpoint.
    r = c.send(request("DELETE", std::string(kAdrf) + sub_location));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(request("DELETE", std::string(kAdrf) + sub_location));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;
    r = c.send(request(
        "POST", std::string(kAdrf) + kDm + "/data-store-records", analytics_record(marker)));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    EXPECT_FALSE(receiver.wait_for("/notify", 3, 3s)) << "notified after unsubscribe";

    // Tidy: the fetch subscription and the records this spec produced (a subscription left
    // behind would keep notifying an endpoint that is gone once this test ends).
    r = c.send(request("DELETE", std::string(kAdrf) + fetch_sub_location));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/remove-stored-data-analytics",
                       json{{"anaSpec", spec}, {"timePeriod", window(3600s, 3600s)}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
}

TEST(Adrf, LifetimeAlertsBeforeDeletionAndHonoursRetrievalInd) {
    Receiver receiver;
    auto lab = spawn_lab();
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + kDm + "/data-store-records", 30s))
        << "ADRF never came up";
    if (!data_store_available()) {
        GTEST_SKIP() << "no Apache Doris behind the ADRF -- skipped, not passed";
    }
    json rec = nrf_data_record("short-lived");
    rec["storeHandl"] = json{
        {"lifetime", 3}, {"delNotifUri", Receiver::url("/alert")}, {"delNotifCorrId", "del-corr"}};
    auto r = c.send(request("POST", std::string(kAdrf) + kDm + "/data-store-records", rec));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string location = r->headers.find("location")->second;
    const std::string id = location.substr(location.rfind('/') + 1);
    const auto stored_at = std::chrono::steady_clock::now();

    ASSERT_TRUE(receiver.wait_for("/alert", 1, 15s)) << "no deletion alert before expiry";
    {
        const auto n = receiver.notes("/alert")[0];
        EXPECT_EQ(n.callback, "Nadrf_DataManagement_storageAlertNotification");
        EXPECT_EQ(n.body["alertStorTransId"], id);
        EXPECT_EQ(n.body["delNotifCorrId"], "del-corr");
    }
    // The receiver answered retrievalInd=true: the record survives its original lifetime by the
    // grace period (4 s, env in spawn_lab) and can still be fetched by the alert's id ...
    std::this_thread::sleep_for(3500ms -
                                std::min(3500ms,
                                         std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - stored_at)));
    r = c.send(
        request("GET", std::string(kAdrf) + kDm + "/data-store-records?store-trans-id=" + id));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 200) << "deleted despite retrievalInd; body: " << r->body;
    // ... and is reaped once the grace period has run out.
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    int status = 200;
    while (std::chrono::steady_clock::now() < deadline && status != 204) {
        std::this_thread::sleep_for(500ms);
        r = c.send(
            request("GET", std::string(kAdrf) + kDm + "/data-store-records?store-trans-id=" + id));
        ASSERT_TRUE(r.has_value()) << r.error();
        status = static_cast<int>(r->status);
    }
    EXPECT_EQ(status, 204) << "the record outlived its deferred expiry";
    EXPECT_EQ(receiver.notes("/alert").size(), 1U) << "alerted more than once";
}

TEST(Adrf, StorageSubscriptionTowardsAnNwdafStoresItsAnalytics) {
    auto lab = spawn_lab();
    setenv("NWDAF_NOTIFICATION_INTERVAL_SECONDS", "2", 1);
    nf_test::SpawnedProcess nwdaf(NWDAF_PATH);
    unsetenv("NWDAF_NOTIFICATION_INTERVAL_SECONDS");
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + kDm + "/data-store-records", 30s))
        << "ADRF never came up";
    ASSERT_TRUE(
        wait_up(c, "https://127.0.0.1:7798/nnwdaf-eventssubscription/v1/subscriptions", 30s))
        << "NWDAF never came up";
    if (!data_store_available()) {
        GTEST_SKIP() << "no Apache Doris behind the ADRF -- skipped, not passed";
    }
    std::this_thread::sleep_for(1500ms); // NRF registrations settle

    // The NWDAF's nfInstanceId, as a consumer would learn it: NRF discovery.
    const auto disc_token = token_as(c, "test-client", "nnrf-disc", "NRF");
    auto disc =
        request("GET",
                std::string(kNrf) +
                    "/nnrf-disc/v1/nf-instances?target-nf-type=NWDAF&requester-nf-type=ADRF",
                {});
    disc.headers.emplace("authorization", "Bearer " + disc_token);
    auto r = c.send(disc);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    const auto instances = json::parse(r->body)["nfInstances"];
    ASSERT_GE(instances.size(), 1U) << r->body;
    const std::string nwdaf_id = instances[0]["nfInstanceId"];

    const std::string set_id = "nwdaf-set-" + std::to_string(std::time(nullptr));
    r = c.send(request(
        "POST",
        std::string(kAdrf) + kDm + "/request-storage-sub",
        json{{"anaSub", json{{"eventSubscriptions", json::array({json{{"event", "NF_LOAD"}}})}}},
             {"targetNfId", nwdaf_id},
             {"dataSetTag", json{{"dataSetId", set_id}}},
             {"storeHandl", json{{"lifetime", 120}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    const std::string trans_ref_id = json::parse(r->body).at("transRefId");
    EXPECT_FALSE(trans_ref_id.empty());

    // A second request for the same spec joins the collection: a new transRefId, and the
    // NWDAF keeps a single subscription (proved below: removing one keeps the data flowing).
    r = c.send(request(
        "POST",
        std::string(kAdrf) + kDm + "/request-storage-sub",
        json{{"anaSub", json{{"eventSubscriptions", json::array({json{{"event", "NF_LOAD"}}})}}},
             {"targetNfId", nwdaf_id}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    const std::string trans_ref_id_2 = json::parse(r->body).at("transRefId");
    EXPECT_NE(trans_ref_id, trans_ref_id_2);

    // The NWDAF notifies every 2 s; the ADRF stores each notification under the data set.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    json got;
    while (std::chrono::steady_clock::now() < deadline) {
        r = c.send(
            request("GET", std::string(kAdrf) + kDm + "/data-store-records?data-set-id=" + set_id));
        ASSERT_TRUE(r.has_value()) << r.error();
        if (r->status == 200) {
            got = json::parse(r->body);
            break;
        }
        std::this_thread::sleep_for(500ms);
    }
    ASSERT_FALSE(got.is_null()) << "the NWDAF's notifications never reached the ADRF's store";
    ASSERT_TRUE(got.contains("anaNotifications")) << got.dump();
    EXPECT_EQ(got["anaNotifications"][0]["eventNotifications"][0]["event"], "NF_LOAD");
    EXPECT_EQ(got["anaSub"][0]["eventSubscriptions"][0]["event"], "NF_LOAD");
    EXPECT_EQ(got["dataSetTag"]["dataSetId"], set_id);

    // Removal: the first leaves, the second still feeds the store; the last closes it.
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/request-storage-sub-removal",
                       json{{"transRefId", trans_ref_id}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/request-storage-sub-removal",
                       json{{"transRefId", trans_ref_id_2}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/request-storage-sub-removal",
                       json{{"transRefId", trans_ref_id_2}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;

    // The NWDAF must no longer hold the ADRF's subscription: its own resource answers 404.
    // (Verified indirectly -- the collection record names the resource; the ADRF deleted it.)
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/remove-stored-data-analytics",
                       json{{"dataSetId", set_id}, {"timePeriod", window(3600s, 3600s)}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
}

TEST(Adrf, StorageSubscriptionThroughTheDccfStoresSourceData) {
#ifdef ADRF_TEST_TSAN
    GTEST_SKIP() << "the MFAF is a librdkafka process; its threads are invisible to "
                    "ThreadSanitizer -- skipped, not passed";
#endif
    const auto b = brokers();
    if (!broker_reachable(b)) {
        GTEST_SKIP() << "no Kafka broker at " << b << " -- DCCF path skipped, not passed";
    }
    auto lab = spawn_lab();
    const std::string topic = "mfaf-adrf-test-" + std::to_string(std::time(nullptr));
    setenv("MFAF_EVENT_BUS_BROKERS", b.c_str(), 1);
    setenv("MFAF_EVENT_BUS_TOPIC", topic.c_str(), 1);
    setenv("MFAF_EVENT_BUS_GROUP_ID", ("mfaf-adrf-test-" + topic).c_str(), 1);
    nf_test::SpawnedProcess mfaf(MFAF_PATH);
    unsetenv("MFAF_EVENT_BUS_BROKERS");
    unsetenv("MFAF_EVENT_BUS_TOPIC");
    unsetenv("MFAF_EVENT_BUS_GROUP_ID");
    nf_test::SpawnedProcess dccf(DCCF_PATH);
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + kDm + "/data-store-records", 30s))
        << "ADRF never came up";
    ASSERT_TRUE(wait_up(c, std::string(kDccf) + "/ndccf-datamanagement/v1/data-subscriptions", 30s))
        << "DCCF never came up";
    ASSERT_TRUE(wait_up(c, "https://127.0.0.1:7800/nmfaf-3dadatamanagement/v1/configurations", 30s))
        << "MFAF never came up";
    if (!data_store_available()) {
        GTEST_SKIP() << "no Apache Doris behind the ADRF -- skipped, not passed";
    }
    std::this_thread::sleep_for(1500ms);

    // NRF NF-status data, coordinated by the DCCF (targetNfSetId -> the DCCF), stored by the
    // ADRF when an NF registers.
    const std::string set_id = "dccf-set-" + std::to_string(std::time(nullptr));
    auto r =
        c.send(request("POST",
                       std::string(kAdrf) + kDm + "/request-storage-sub",
                       json{{"dataSub",
                             json{{"nrfDataSub",
                                   json{{"nfStatusNotificationUri", "https://placeholder.invalid/"},
                                        {"subscrCond", json{{"nfType", "NSACF"}}}}}}},
                            {"targetNfSetId", "set1.dccfset.5gc.mnc001.mcc001"},
                            {"dataSetTag", json{{"dataSetId", set_id}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    const std::string trans_ref_id = json::parse(r->body).at("transRefId");

    nf_test::SpawnedProcess nsacf(NSACF_PATH); // registers with the NRF -> NF_REGISTERED
    const auto deadline = std::chrono::steady_clock::now() + 40s;
    json got;
    while (std::chrono::steady_clock::now() < deadline) {
        r = c.send(
            request("GET", std::string(kAdrf) + kDm + "/data-store-records?data-set-id=" + set_id));
        ASSERT_TRUE(r.has_value()) << r.error();
        if (r->status == 200) {
            got = json::parse(r->body);
            break;
        }
        std::this_thread::sleep_for(500ms);
    }
    ASSERT_FALSE(got.is_null()) << "the NRF's NFStatusNotify never reached the ADRF via DCCF/MFAF";
    ASSERT_TRUE(got.contains("dataNotif")) << got.dump();
    ASSERT_TRUE(got["dataNotif"].contains("nrfEventNotifs")) << got.dump();
    EXPECT_EQ(got["dataNotif"]["nrfEventNotifs"][0]["event"], "NF_REGISTERED");
    ASSERT_TRUE(got["dataSub"][0].contains("nrfDataSub")) << got.dump();

    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/request-storage-sub-removal",
                       json{{"dataSetId", set_id}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(request("POST",
                       std::string(kAdrf) + kDm + "/remove-stored-data-analytics",
                       json{{"dataSetId", set_id}, {"timePeriod", window(3600s, 3600s)}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
}

TEST(Adrf, MlModelsStoredServedAndAccessControlled) {
    Receiver receiver;
    auto lab = spawn_lab();
    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + kMl + "/mlmodel-store-records", 30s))
        << "ADRF never came up";
    std::this_thread::sleep_for(1s);
    const auto owner = token_as(c, "mtlf-owner", "nadrf-mlmodelmanagement", "ADRF");
    const auto allowed = token_as(c, "anlf-allowed", "nadrf-mlmodelmanagement", "ADRF");
    const auto stranger = token_as(c, "anlf-stranger", "nadrf-mlmodelmanagement", "ADRF");
    ASSERT_FALSE(owner.empty());
    ASSERT_FALSE(allowed.empty());
    ASSERT_FALSE(stranger.empty());
    const auto as = [&](sbi_core::http2::ClientRequest req, const std::string& token) {
        req.headers.emplace("authorization", "Bearer " + token);
        return req;
    };
    const std::string model_id = std::to_string(std::time(nullptr) % 1000000000);
    const std::int64_t id_inline = std::stoll(model_id) * 10 + 1;
    const std::int64_t id_download = std::stoll(model_id) * 10 + 2;

    // Inline model (base64 in mlModel), allowed for one other consumer.
    auto r = c.send(as(
        request(
            "POST",
            std::string(kAdrf) + kMl + "/mlmodel-store-records",
            json{{"nfInstanceId", "mtlf-owner"},
                 {"mlModels",
                  json::array({json{{"modelUniqueId", id_inline},
                                    {"mlModel", "T05OWC1ieXRlcw=="}, // "ONNX-bytes"
                                    {"allowConsumerList",
                                     json::array({json{{"nfInstanceId", "anlf-allowed"}}})}}})}}),
        owner));
    if (r && r->status == 500 && r->body.find("ML model store") != std::string::npos) {
        GTEST_SKIP() << "no PostgreSQL behind the ADRF's ML model store -- skipped, not passed: "
                     << r->body;
    }
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto created = json::parse(r->body);
    const std::string record_location = r->headers.find("location")->second;
    ASSERT_EQ(created["mlModelInfo"].size(), 1U) << created.dump();
    EXPECT_EQ(created["mlModelInfo"][0]["mlStorageSize"], 10);
    EXPECT_EQ(created["suppFeat"], "1"); // EnModelMgmt
    const std::string file_url = created["mlModelInfo"][0]["mlFileAddr"]["mLModelUrl"];
    ASSERT_NE(file_url.find(std::string(kAdrf) + "/adrf-mlmodel-files/v1/"), std::string::npos);

    // The file, served by the ADRF: owner and allowed consumer get it, a stranger does not.
    r = c.send(as(request("GET", file_url, {}), owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(r->body, "ONNX-bytes");
    r = c.send(as(request("GET", file_url, {}), allowed));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 200) << r->body;
    r = c.send(as(request("GET", file_url, {}), stranger));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 403) << r->body;
    EXPECT_EQ(json::parse(r->body)["cause"], "RETRIEVAL_ML_MODEL_NOT_ALLOWED");

    // RetrievalRequest by unique id: same rule.
    const std::string get_by_id =
        std::string(kAdrf) + kMl +
        "/mlmodel-store-records?model-unique-ids=" + std::to_string(id_inline);
    r = c.send(as(request("GET", get_by_id, {}), allowed));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)["mlModelInfo"][0]["mlFileAddr"]["mLModelUrl"], file_url);
    r = c.send(as(request("GET", get_by_id, {}), stranger));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 403) << r->body;

    // Download path: mlModelInfo naming a file on the consumer's server; and one that is not
    // there -> all failed for one reason -> 404 ML_MODEL_FILE_ADDRESS_NOT_FOUND.
    r = c.send(as(
        request("POST",
                std::string(kAdrf) + kMl + "/mlmodel-store-records",
                json{{"nfInstanceId", "mtlf-owner"},
                     {"mlModelInfo",
                      json::array(
                          {json{{"modelUniqueId", id_download},
                                {"mlFileAddr", json{{"mLModelUrl", Receiver::url("/model.bin")}}},
                                {"mlStorageSize", 20}}})}}),
        owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    EXPECT_EQ(json::parse(r->body)["mlModelInfo"][0]["mlStorageSize"], 20);
    const std::string download_location = r->headers.find("location")->second;
    r = c.send(as(
        request("POST",
                std::string(kAdrf) + kMl + "/mlmodel-store-records",
                json{{"nfInstanceId", "mtlf-owner"},
                     {"mlModelInfo",
                      json::array(
                          {json{{"modelUniqueId", id_download + 7},
                                {"mlFileAddr", json{{"mLModelUrl", Receiver::url("/missing.bin")}}},
                                {"mlStorageSize", 1}}})}}),
        owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;
    EXPECT_EQ(json::parse(r->body)["cause"], "ML_MODEL_FILE_ADDRESS_NOT_FOUND");

    // Update: PUT re-stores the inline model with a wider allow list; the stranger now passes.
    r = c.send(as(
        request(
            "PUT",
            std::string(kAdrf) + record_location,
            json{{"nfInstanceId", "mtlf-owner"},
                 {"mlModels",
                  json::array({json{{"modelUniqueId", id_inline},
                                    {"mlModel", "T05OWC1ieXRlcy12Mg=="}, // "ONNX-bytes-v2"
                                    {"allowConsumerList",
                                     json::array({json{{"nfInstanceId", "anlf-allowed"}},
                                                  json{{"nfInstanceId", "anlf-stranger"}}})}}})}}),
        owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    r = c.send(as(request("GET", file_url, {}), stranger));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(r->body, "ONNX-bytes-v2");

    // Delete by record: per-model results; then by unique id: nothing left -> 404.
    r = c.send(as(request("DELETE", std::string(kAdrf) + record_location, {}), owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)[0]["deleteResult"], "ML_MODEL_DELETED");
    r = c.send(as(request("GET", file_url, {}), owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;
    r = c.send(as(request("POST",
                          std::string(kAdrf) + kMl + "/remove-stored-mlmodel",
                          json::array({id_inline})),
                  owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;
    EXPECT_EQ(json::parse(r->body)["cause"], "ML_MODEL_NOT_FOUND");
    // Partial: one present (the downloaded model), one not -> 200 with both results.
    r = c.send(as(request("POST",
                          std::string(kAdrf) + kMl + "/remove-stored-mlmodel",
                          json::array({id_download, id_inline})),
                  owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    {
        const auto results = json::parse(r->body);
        EXPECT_EQ(results[0]["deleteResult"], "ML_MODEL_DELETED");
        EXPECT_EQ(results[1]["deleteResult"], "ML_MODEL_NOT_FOUND");
    }
    r = c.send(as(request("DELETE", std::string(kAdrf) + download_location, {}), owner));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 200) << r->body; // the (now empty) record itself
}
