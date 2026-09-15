// NWDAF containing MTLF -- TS 23.288 6.2A / 6.2B.5 / 6.2B.7 / 6.2E.2, TS 29.520 4.5 (ADR-0369).
//
// One lab: NRF, ADRF (Doris + PostgreSQL + Valkey behind it), an NWDAF in role anlf (7798) and
// an NWDAF in role mtlf (7797); the training sidecar (nfs/nwdaf/training/train_nf_load.py) runs
// as the MTLF's subprocess. The test is a third ML-model consumer with its own notifUri.
//
//   1. Nnwdaf_MLModelProvision_Subscribe: an event this MTLF does not train is refused with the
//      spec's 500 UNAVAILABLE_ML_MODEL_FOR_ALLEVENTS; a mixed request is accepted with
//      failEventReports; NF_LOAD alone is 201 with a Location.
//   2. The MTLF trains (the ADRF data set is empty: the sidecar's labelled synthetic bootstrap),
//      stores the ONNX through Nadrf_MLModelManagement, and notifies: mLModelAdrf with the
//      ADRF's nfInstanceId and storTransId, mLFileAddr, modelUniqueId, modelProviderId,
//      addModelInfo with the ACCURACY metric, the held-out accuracy and the training lineage.
//   3. The consumer retrieves the model from the ADRF (it is on the allow list because it
//      subscribed): real ONNX bytes.
//   4. NRF observations with a `load` are injected into the AnLF's collection (its inbound
//      delivery endpoint, as the MFAF would); the AnLF's Nnwdaf_DataManagement subscription from
//      the ADRF (opened by the MTLF's storage-subscription request, 6.2E.2 step 8) stores them;
//      the MTLF sees the data set grow and RE-TRAINS on real observations: a second notification
//      with modelUpdateInd and data_source "adrf".
//   5. The AnLF took the model into use: an NF_LOAD request for a FUTURE period (ana-req.endTs)
//      answers predictions with `confidence` (TS 23.288 Table 6.5.3-2); the same request without
//      a future period stays statistical (no confidence).
//
// Skips (not passes) without Doris behind the ADRF or without the sidecar's interpreter.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>
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
constexpr const char* kAnlf = "https://127.0.0.1:7798";
constexpr const char* kMtlf = "https://127.0.0.1:7797";
constexpr const char* kAdrf = "https://127.0.0.1:7804";
constexpr const char* kProv = "/nnwdaf-mlmodelprovision/v1";
constexpr int kReceiverPort = 19993;

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

std::string token_for(sbi_core::http2::Client& c, const std::string& scope, const std::string& nf) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string(kNrf) + "/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=" + scope +
               "&targetNfType=" + nf;
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

std::string url_encode(const std::string& s) {
    std::string out;
    char buf[4];
    for (const unsigned char ch : s) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            std::snprintf(buf, sizeof buf, "%%%02X", ch);
            out += buf;
        }
    }
    return out;
}

class Receiver {
public:
    Receiver()
        : server_(ioc_,
                  "127.0.0.1",
                  kReceiverPort,
                  sbi_core::http2::TlsConfig{.cert_path = CERTS_DIR "/hello-nf/cert.pem",
                                             .key_path = CERTS_DIR "/hello-nf/key.pem",
                                             .ca_path = CERTS_DIR "/ca/ca.crt"}) {
        server_.add_route("POST", "/ml", [this](const sbi_core::http2::Request& req) {
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
    static std::string url() {
        return "https://127.0.0.1:" + std::to_string(kReceiverPort) + "/ml";
    }
    template <typename Pred> bool wait_until(std::chrono::seconds limit, Pred pred) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [&] {
            for (const auto& n : notes_) {
                if (pred(n)) {
                    return true;
                }
            }
            return false;
        });
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

// The first MLEventNotif for NF_LOAD in a notification (array of NwdafMLModelProvNotif).
json nf_load_notif(const json& body) {
    for (const auto& n : body.is_array() ? body : json::array({body})) {
        for (const auto& en : n.value("eventNotifs", json::array())) {
            if (en.value("event", "") == "NF_LOAD") {
                return en;
            }
        }
    }
    return json();
}

json stats_of(const json& en) {
    for (const auto& add : en.value("addModelInfo", json::array())) {
        for (const auto& t : add.value("trainInpInfos", json::array())) {
            if (t.contains("dataStatisticsInfos")) {
                return json::parse(t.at("dataStatisticsInfos").get<std::string>());
            }
        }
    }
    return json();
}

std::string redis_url() {
    if (const char* env = std::getenv("NWDAF_REDIS_URL")) {
        return env;
    }
    return "tcp://127.0.0.1:6379";
}

std::string training_python() {
    if (const char* env = std::getenv("NWDAF_TRAINING_PYTHON")) {
        return env;
    }
    return std::string(NWDAF_TRAINING_DIR) + "/.venv/bin/python3";
}

} // namespace

TEST(NwdafMtlf, TrainsStoresProvisionsAndTheAnlfPredicts) {
    const auto python = training_python();
    if (!std::filesystem::exists(python)) {
        GTEST_SKIP() << "no training sidecar interpreter at " << python
                     << " (nfs/nwdaf/training/README.md) -- skipped, not passed";
    }
    // ML-model state from an earlier run would make "first training" untrue: clear it. The
    // MTLF's storage subscription at the ADRF (reused across MTLF restarts by design) is removed
    // at the ADRF once it is up, so this run's data set is fed by a collection of its own.
    std::string stale_storage_sub;
    try {
        sw::redis::Redis redis(redis_url());
        if (const auto v = redis.get("nwdaf:mlstoragesub:NF_LOAD")) {
            stale_storage_sub = *v;
        }
        for (const char* k : {"nwdaf:mlmodel:NF_LOAD",
                              "nwdaf:mlstoragesub:NF_LOAD",
                              "nwdaf:mltrain:NF_LOAD",
                              "nwdaf:anlf:model:NF_LOAD",
                              "nwdaf:anlf:mlsub:NF_LOAD",
                              "nwdaf:anlf:mlsub:NF_LOAD:alive"}) {
            redis.del(k);
        }
        std::vector<std::string> subs;
        redis.smembers("nwdaf:mlprov:subs", std::back_inserter(subs));
        for (const auto& id : subs) {
            redis.del("nwdaf:mlprov:sub:" + id);
        }
        redis.del("nwdaf:mlprov:subs");
    } catch (const std::exception& e) {
        GTEST_SKIP() << "no Valkey at " << redis_url() << " (" << e.what()
                     << ") -- skipped, not passed";
    }

    Receiver receiver;
    auto c = make_client();
    const std::string run_id = std::to_string(std::time(nullptr));
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_TRUE(wait_up(c, std::string(kNrf) + "/nnrf-nfm/v1/nf-instances", 30s));
    nf_test::SpawnedProcess adrf(ADRF_PATH);
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + "/nadrf-datamanagement/v1/data-store-records", 30s))
        << "ADRF never came up";
    {
        auto r = c.send(request(
            "GET",
            std::string(kAdrf) + "/nadrf-datamanagement/v1/data-store-records?store-trans-id=x"));
        if (r && r->status == 500 && r->body.find("data store unreachable") != std::string::npos) {
            GTEST_SKIP() << "no Apache Doris behind the ADRF -- skipped, not passed";
        }
    }
    const auto remove_storage_sub = [&](const std::string& trans_ref_id) {
        if (trans_ref_id.empty()) {
            return;
        }
        auto rr = c.send(
            request("POST",
                    std::string(kAdrf) + "/nadrf-datamanagement/v1/request-storage-sub-removal",
                    json{{"transRefId", trans_ref_id}}));
        EXPECT_TRUE(rr.has_value() && (rr->status == 204 || rr->status == 404))
            << (rr ? rr->body : rr.error());
    };
    remove_storage_sub(stale_storage_sub);

    // The AnLF: no DCCF in this lab (observations are injected below), fast notifications.
    setenv("NWDAF_ROLE", "anlf", 1);
    setenv("NWDAF_DATA_COLLECTION_VIA_DCCF", "false", 1);
    setenv("NWDAF_NOTIFICATION_INTERVAL_SECONDS", "2", 1);
    setenv("NWDAF_ML_MODEL_PROVISION_RETRY_SECONDS", "2", 1);
    setenv("NWDAF_ML_MODEL_PROVISION_HOLDER_HEARTBEAT_SECONDS", "2", 1);
    nf_test::SpawnedProcess anlf(NWDAF_PATH);
    // The MTLF: its own port, a run-unique data set, a small sample bar so the injected
    // observations count, the real sidecar.
    setenv("NWDAF_ROLE", "mtlf", 1);
    setenv("NWDAF_PORT", "7797", 1);
    setenv("NWDAF_METRICS_BIND_ADDRESS", "0.0.0.0:9486", 1);
    setenv("NWDAF_SELF_BASE_URL", kMtlf, 1);
    setenv("NWDAF_MTLF_DATA_SET_ID", ("mtlf-test-" + run_id).c_str(), 1);
    setenv("NWDAF_MTLF_MIN_SAMPLES", "40", 1);
    setenv("NWDAF_MTLF_RETRAIN_MIN_NEW_WINDOWS", "40", 1);
    setenv("NWDAF_MTLF_CHECK_INTERVAL_SECONDS", "2", 1);
    setenv("NWDAF_TRAINING_PYTHON", python.c_str(), 1);
    nf_test::SpawnedProcess mtlf(NWDAF_PATH);
    for (const char* k : {"NWDAF_ROLE",
                          "NWDAF_PORT",
                          "NWDAF_METRICS_BIND_ADDRESS",
                          "NWDAF_SELF_BASE_URL",
                          "NWDAF_MTLF_DATA_SET_ID",
                          "NWDAF_MTLF_MIN_SAMPLES",
                          "NWDAF_MTLF_RETRAIN_MIN_NEW_WINDOWS",
                          "NWDAF_MTLF_CHECK_INTERVAL_SECONDS",
                          "NWDAF_TRAINING_PYTHON",
                          "NWDAF_DATA_COLLECTION_VIA_DCCF",
                          "NWDAF_NOTIFICATION_INTERVAL_SECONDS",
                          "NWDAF_ML_MODEL_PROVISION_RETRY_SECONDS",
                          "NWDAF_ML_MODEL_PROVISION_HOLDER_HEARTBEAT_SECONDS"}) {
        unsetenv(k);
    }
    ASSERT_TRUE(wait_up(c, std::string(kAnlf) + "/nnwdaf-analyticsinfo/v1/analytics", 30s))
        << "AnLF never came up";
    ASSERT_TRUE(wait_up(c, std::string(kMtlf) + kProv + "/subscriptions", 30s))
        << "MTLF never came up";
    std::this_thread::sleep_for(2s); // NRF registrations settle

    const auto token = token_for(c, "nnwdaf-mlmodelprovision", "NWDAF");
    ASSERT_FALSE(token.empty());
    const auto as_consumer = [&](sbi_core::http2::ClientRequest req) {
        req.headers.emplace("authorization", "Bearer " + token);
        return req;
    };

    // 1. Subscribe: refused, mixed, accepted.
    auto r = c.send(as_consumer(request(
        "POST",
        std::string(kMtlf) + kProv + "/subscriptions",
        json{{"mLEventSubscs",
              json::array({json{{"mLEvent", "UE_MOBILITY"}, {"mLEventFilter", json::object()}}})},
             {"notifUri", Receiver::url()}})));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 500) << r->body;
    EXPECT_EQ(json::parse(r->body).value("cause", ""), "UNAVAILABLE_ML_MODEL_FOR_ALLEVENTS");

    r = c.send(as_consumer(request(
        "POST",
        std::string(kMtlf) + kProv + "/subscriptions",
        json{{"mLEventSubscs",
              json::array({json{{"mLEvent", "NF_LOAD"}, {"mLEventFilter", json::object()}},
                           json{{"mLEvent", "UE_MOBILITY"}, {"mLEventFilter", json::object()}}})},
             {"notifUri", Receiver::url()}})));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    {
        const auto body = json::parse(r->body);
        ASSERT_TRUE(body.contains("failEventReports")) << r->body;
        EXPECT_EQ(body["failEventReports"][0]["event"], "UE_MOBILITY");
        EXPECT_EQ(body["failEventReports"][0]["failureCode"], "UNAVAILABLE_ML_MODEL");
    }
    std::string mixed_location;
    if (const auto it = r->headers.find("location"); it != r->headers.end()) {
        mixed_location = it->second;
    }
    ASSERT_FALSE(mixed_location.empty());
    r = c.send(as_consumer(request("DELETE", std::string(kMtlf) + mixed_location)));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;

    r = c.send(as_consumer(request("POST",
                                   std::string(kMtlf) + kProv + "/subscriptions",
                                   json{{"mLEventSubscs",
                                         json::array({json{{"mLEvent", "NF_LOAD"},
                                                           {"mLEventFilter", json::object()},
                                                           {"mlEvRepCon",
                                                            json{{"modelMetric", "ACCURACY"},
                                                                 {"mlAccuracyThreshold", 80}}}}})},
                                        {"notifUri", Receiver::url()},
                                        {"notifCorreId", "test-nf-load"}})));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    std::string location;
    if (const auto it = r->headers.find("location"); it != r->headers.end()) {
        location = it->second;
    }
    ASSERT_NE(location.find(std::string(kProv) + "/subscriptions/"), std::string::npos) << location;

    // 2. The first model: trained (synthetic bootstrap -- the data set is empty), stored,
    //    notified.
    ASSERT_TRUE(receiver.wait_until(120s, [](const Receiver::Note& n) {
        return !nf_load_notif(n.body).is_null();
    })) << "no Nnwdaf_MLModelProvision_Notify within 120 s";
    const auto first = receiver.notes().front();
    EXPECT_EQ(first.callback, "Nnwdaf_MLModelProvision_myNotification");
    ASSERT_TRUE(first.body.is_array()) << first.body.dump();
    EXPECT_NE(first.body[0].value("subscriptionId", "").find("nwdaf-ml-"), std::string::npos);
    const auto en = nf_load_notif(first.body);
    EXPECT_EQ(en.value("notifCorreId", ""), "test-nf-load");
    ASSERT_TRUE(en.contains("modelUniqueId")) << en.dump();
    ASSERT_TRUE(en.contains("mLModelAdrf")) << en.dump();
    EXPECT_FALSE(en["mLModelAdrf"].value("adrfId", "").empty()) << en.dump();
    EXPECT_FALSE(en["mLModelAdrf"].value("storTransId", "").empty()) << en.dump();
    ASSERT_TRUE(en.contains("mLFileAddr")) << en.dump();
    EXPECT_FALSE(en.value("modelProviderId", "").empty());
    EXPECT_FALSE(en.value("modelUpdateInd", true));
    ASSERT_TRUE(en.contains("addModelInfo")) << en.dump();
    EXPECT_EQ(en["addModelInfo"][0]["modelMetric"], "ACCURACY");
    EXPECT_TRUE(en["addModelInfo"][0].contains("accMLModel"));
    EXPECT_EQ(en["addModelInfo"][0]["modelUniqueId"], en["modelUniqueId"]);
    const auto first_stats = stats_of(en);
    ASSERT_FALSE(first_stats.is_null()) << en.dump();
    EXPECT_EQ(first_stats.value("data_source", ""), "synthetic_bootstrap") << first_stats.dump();
    EXPECT_FALSE(first_stats.value("mlflow_run_id", "").empty()) << first_stats.dump();
    const std::string file_url = en["mLFileAddr"].value("mLModelUrl", "");
    ASSERT_NE(file_url.find(std::string(kAdrf) + "/adrf-mlmodel-files/v1/"), std::string::npos)
        << file_url;

    // 3. Retrieve the model from the ADRF as the subscribed consumer.
    const auto adrf_token = token_for(c, "nadrf-mlmodelmanagement", "ADRF");
    ASSERT_FALSE(adrf_token.empty());
    {
        auto get = request("GET", file_url);
        get.headers.emplace("authorization", "Bearer " + adrf_token);
        r = c.send(get);
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 200) << r->body;
        EXPECT_GT(r->body.size(), 1000U);
        EXPECT_EQ(static_cast<unsigned char>(r->body[0]), 0x08U); // ONNX ModelProto ir_version
    }

    // 4. Observations with a load reach the AnLF's collection; the ADRF stores them through the
    //    AnLF's Nnwdaf_DataManagement; the MTLF re-trains on them.
    const std::string tail = (run_id + "000000000000").substr(0, 12); // UUID-shaped, run-unique
    const std::vector<std::string> instances{"11111111-1111-4111-8111-" + tail,
                                             "22222222-2222-4222-8222-" + tail,
                                             "33333333-3333-4333-8333-" + tail};
    for (int step = 0; step < 30; ++step) {
        json notifs = json::array();
        for (std::size_t k = 0; k < instances.size(); ++k) {
            const int load =
                30 + static_cast<int>(k) * 15 + ((step * 7 + static_cast<int>(k) * 3) % 11);
            notifs.push_back(json{
                {"event", step == 0 ? "NF_REGISTERED" : "NF_PROFILE_CHANGED"},
                {"nfInstanceUri", std::string(kNrf) + "/nnrf-nfm/v1/nf-instances/" + instances[k]},
                {"nfProfile",
                 json{{"nfInstanceId", instances[k]},
                      {"nfType", "AMF"},
                      {"nfStatus", "REGISTERED"},
                      {"load", load}}}});
        }
        r = c.send(
            request("POST",
                    std::string(kAnlf) + "/nwdaf-inbound/v1/notifications/nrf",
                    json{{"dataAnaNotif", json{{"dataNotif", json{{"nrfEventNotifs", notifs}}}}}}));
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 204) << r->body;
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(receiver.wait_until(150s, [](const Receiver::Note& n) {
        const auto e = nf_load_notif(n.body);
        return !e.is_null() && e.value("modelUpdateInd", false);
    })) << "the MTLF never re-trained on the stored observations within 150 s";
    json updated;
    for (const auto& n : receiver.notes()) {
        const auto e = nf_load_notif(n.body);
        if (!e.is_null() && e.value("modelUpdateInd", false)) {
            updated = e;
        }
    }
    const auto second_stats = stats_of(updated);
    EXPECT_EQ(second_stats.value("data_source", ""), "adrf") << second_stats.dump();
    EXPECT_GE(second_stats.value("n_real_windows", 0), 40) << second_stats.dump();
    EXPECT_NE(updated["modelUniqueId"], en["modelUniqueId"]);
    EXPECT_NE(second_stats.value("mlflow_run_id", ""), first_stats.value("mlflow_run_id", ""));

    // 5. The AnLF predicts: a future period carries confidence; a statistical request does not.
    const auto disc_token = token_for(c, "nnwdaf-analyticsinfo", "NWDAF");
    const std::string filter = url_encode(json{{"nfInstanceIds", instances}}.dump());
    const std::string future = url_encode(json{{"endTs", now_plus(600s)}}.dump());
    json predicted;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto get =
            request("GET",
                    std::string(kAnlf) + "/nnwdaf-analyticsinfo/v1/analytics?event-id=NF_LOAD" +
                        "&event-filter=" + filter + "&ana-req=" + future);
        get.headers.emplace("authorization", "Bearer " + disc_token);
        r = c.send(get);
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 200) << r->body;
        const auto body = json::parse(r->body);
        bool all = !body.value("nfLoadLevelInfos", json::array()).empty();
        for (const auto& i : body.value("nfLoadLevelInfos", json::array())) {
            all &= i.contains("confidence");
        }
        if (all) {
            predicted = body;
            break;
        }
        std::this_thread::sleep_for(1s);
    }
    ASSERT_FALSE(predicted.is_null()) << "the AnLF never answered NF_LOAD predictions: " << r->body;
    EXPECT_TRUE(predicted.contains("expiry")) << predicted.dump();
    for (const auto& i : predicted["nfLoadLevelInfos"]) {
        EXPECT_GE(i.value("nfLoadLevelAverage", -1), 0) << i.dump();
        EXPECT_LE(i.value("nfLoadLevelAverage", 101), 100) << i.dump();
        EXPECT_FALSE(i.contains("nfLoadLevelpeak")) << i.dump();
        EXPECT_GE(i.value("confidence", -1), 0) << i.dump();
    }
    {
        auto get =
            request("GET",
                    std::string(kAnlf) + "/nnwdaf-analyticsinfo/v1/analytics?event-id=NF_LOAD" +
                        "&event-filter=" + filter);
        get.headers.emplace("authorization", "Bearer " + disc_token);
        r = c.send(get);
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 200) << r->body;
        const auto body = json::parse(r->body);
        ASSERT_FALSE(body.value("nfLoadLevelInfos", json::array()).empty()) << r->body;
        for (const auto& i : body["nfLoadLevelInfos"]) {
            EXPECT_FALSE(i.contains("confidence")) << i.dump();
            EXPECT_TRUE(i.contains("nfLoadLevelpeak")) << i.dump();
        }
    }

    // Unsubscribe; a second delete is the YAML's 404.
    r = c.send(as_consumer(request("DELETE", std::string(kMtlf) + location)));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(as_consumer(request("DELETE", std::string(kMtlf) + location)));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;
    // This run's storage subscription and stored observations leave with it.
    try {
        sw::redis::Redis redis(redis_url());
        if (const auto v = redis.get("nwdaf:mlstoragesub:NF_LOAD")) {
            remove_storage_sub(*v);
            redis.del("nwdaf:mlstoragesub:NF_LOAD");
        }
    } catch (const std::exception& e) {
        ADD_FAILURE() << e.what();
    }
    r = c.send(
        request("POST",
                std::string(kAdrf) + "/nadrf-datamanagement/v1/remove-stored-data-analytics",
                json{{"dataSetId", "mtlf-test-" + run_id},
                     {"timePeriod",
                      json{{"startTime", now_plus(-3600s)}, {"stopTime", now_plus(3600s)}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
}
