// NWDAF Phase C end to end (ADR-0368): data collection via the DCCF and Nnwdaf_DataManagement,
// against a real NRF, MFAF (Kafka), DCCF, Valkey -- and, for the storage path, a real ADRF on
// Doris/PostgreSQL.
//
//   * Collection (TS 23.288 6.2.6.3.4): the NWDAF subscribes NRF NF-status data at the DCCF on
//     its own; an NF registering at the NRF reaches the NWDAF through DCCF -> MFAF, and is served
//     to an Nnwdaf_DataManagement consumer as a DataNotification with nrfEventNotifs.
//   * NF_LOAD from collected data (TS 29.520 NfStatus = "percentage of time spent on various NF
//     states"): after the NF is deregistered at the NRF its status shares are time-weighted --
//     registered below 100%, unregistered present -- and it is still reported although the NRF
//     snapshot no longer lists it.
//   * Nnwdaf_DataManagement (TS 29.520 4.4): historical delivery for a timePeriod, fetch
//     instructions under consTrigNotif redeemed once at the fetchUri, update, unsubscribe, and
//     SUBSCRIPTION_CANNOT_BE_SERVED for data this NWDAF does not collect.
//   * ADRF (TS 29.575 4.2.2.3.2 with an NWDAF target): a dataSub storage subscription is served
//     by Nnwdaf_DataManagement and the collected NRF data lands in the ADRF's store.
//
// Skipped, not passed, without a Kafka broker and under ThreadSanitizer (the MFAF is a librdkafka
// process; ADR-0364 CI note 3); the ADRF test also without Doris.
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
#define NWDAF_C_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NWDAF_C_TEST_TSAN 1
#endif
#endif

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr const char* kNrf = "https://127.0.0.1:7777";
constexpr const char* kNwdaf = "https://127.0.0.1:7798";
constexpr const char* kDccf = "https://127.0.0.1:7803";
constexpr const char* kMfaf = "https://127.0.0.1:7800";
constexpr const char* kAdrf = "https://127.0.0.1:7804";
constexpr const char* kDm = "/nnwdaf-datamanagement/v1";
constexpr int kReceiverPort = 19992;

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

class Receiver {
public:
    Receiver()
        : server_(ioc_,
                  "127.0.0.1",
                  kReceiverPort,
                  sbi_core::http2::TlsConfig{.cert_path = CERTS_DIR "/hello-nf/cert.pem",
                                             .key_path = CERTS_DIR "/hello-nf/key.pem",
                                             .ca_path = CERTS_DIR "/ca/ca.crt"}) {
        for (const char* path : {"/dm", "/dm-history", "/dm-fetch"}) {
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
                resp.status = 204;
                return resp;
            });
        }
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
    // Waits until a notification on `path` satisfies `pred`.
    template <typename Pred>
    bool wait_until(const char* path, std::chrono::seconds limit, Pred pred) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [&] {
            for (const auto& n : notes_) {
                if (n.path == path && pred(n)) {
                    return true;
                }
            }
            return false;
        });
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
    boost::asio::io_context ioc_;
    sbi_core::http2::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Note> notes_;
};

// Does a NnwdafDataManagementNotif carry an NRF event of `event` for an NF of `nf_type`?
bool carries(const json& notif, const std::string& event, const std::string& nf_type) {
    for (const auto& e :
         notif.value("dataNotification", json::object()).value("nrfEventNotifs", json::array())) {
        if (e.value("event", "") == event &&
            e.value("nfProfile", json::object()).value("nfType", "") == nf_type) {
            return true;
        }
    }
    return false;
}

struct Lab {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess mfaf;
    nf_test::SpawnedProcess dccf;
    nf_test::SpawnedProcess nwdaf;
};

// NRF, MFAF (on the CI/lab Kafka), DCCF, then the NWDAF -- in that order, so the NWDAF's own
// subscription at the DCCF succeeds on its first attempt.
Lab spawn_lab(sbi_core::http2::Client& c, const std::string& b) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    const std::string topic = "mfaf-nwdaf-c-test-" + std::to_string(std::time(nullptr));
    setenv("MFAF_EVENT_BUS_BROKERS", b.c_str(), 1);
    setenv("MFAF_EVENT_BUS_TOPIC", topic.c_str(), 1);
    setenv("MFAF_EVENT_BUS_GROUP_ID", ("mfaf-nwdaf-c-" + topic).c_str(), 1);
    nf_test::SpawnedProcess mfaf(MFAF_PATH);
    unsetenv("MFAF_EVENT_BUS_BROKERS");
    unsetenv("MFAF_EVENT_BUS_TOPIC");
    unsetenv("MFAF_EVENT_BUS_GROUP_ID");
    nf_test::SpawnedProcess dccf(DCCF_PATH);
    wait_up(c, std::string(kDccf) + "/ndccf-datamanagement/v1/data-subscriptions", 30s);
    wait_up(c, std::string(kMfaf) + "/nmfaf-3dadatamanagement/v1/configurations", 30s);
    nf_test::SpawnedProcess nwdaf(NWDAF_PATH);
    return Lab{std::move(nrf), std::move(mfaf), std::move(dccf), std::move(nwdaf)};
}

std::string nf_instance_id_of(sbi_core::http2::Client& c, const std::string& nf_type) {
    const auto token = token_for(c, "nnrf-disc", "NRF");
    auto disc = request("GET",
                        std::string(kNrf) + "/nnrf-disc/v1/nf-instances?target-nf-type=" + nf_type +
                            "&requester-nf-type=NWDAF");
    disc.headers.emplace("authorization", "Bearer " + token);
    auto r = c.send(disc);
    if (!r || r->status != 200) {
        return "";
    }
    const auto instances = json::parse(r->body).value("nfInstances", json::array());
    return instances.empty() ? "" : instances[0].value("nfInstanceId", "");
}

} // namespace

TEST(NwdafPhaseC, CollectsNrfDataViaTheDccfAndServesItOverDataManagement) {
#ifdef NWDAF_C_TEST_TSAN
    GTEST_SKIP() << "the MFAF is a librdkafka process; its threads are invisible to "
                    "ThreadSanitizer -- skipped, not passed";
#endif
    const auto b = brokers();
    if (!broker_reachable(b)) {
        GTEST_SKIP() << "no Kafka broker at " << b << " -- skipped, not passed";
    }
    Receiver receiver;
    auto c = make_client();
    auto lab = spawn_lab(c, b);
    ASSERT_TRUE(wait_up(c, std::string(kNwdaf) + kDm + "/subscriptions", 30s))
        << "NWDAF never came up";
    std::this_thread::sleep_for(2s); // NRF registrations, tokens, the NWDAF's DCCF subscription

    // ---- refusals: data this NWDAF does not collect, per-UE without consent -------------------
    auto r = c.send(request(
        "POST",
        std::string(kNwdaf) + kDm + "/subscriptions",
        json{{"notifCorrId", "x"},
             {"notificURI", Receiver::url("/dm")},
             {"dataSub",
              json{{"smfDataSub",
                    json{{"notifId", "x"},
                         {"notifUri", "https://placeholder.invalid/"},
                         {"eventSubs", json::array({json{{"event", "PDU_SES_REL"}}})}}}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 400) << r->body;
    EXPECT_EQ(json::parse(r->body)["cause"], "SUBSCRIPTION_CANNOT_BE_SERVED");
    r = c.send(request(
        "POST",
        std::string(kNwdaf) + kDm + "/subscriptions",
        json{{"notifCorrId", "x"},
             {"notificURI", Receiver::url("/dm")},
             {"anaSub",
              json{{"eventSubscriptions",
                    json::array({json{
                        {"event", "NF_LOAD"},
                        {"tgtUe", json{{"supis", json::array({"imsi-999700000000001"})}}}}})}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 403) << r->body;
    EXPECT_EQ(json::parse(r->body)["cause"], "USER_CONSENT_NOT_GRANTED");

    // ---- a consumer of the NRF data, then an NF appears -------------------------------------
    r = c.send(
        request("POST",
                std::string(kNwdaf) + kDm + "/subscriptions",
                json{{"notifCorrId", "dm-1"},
                     {"notificURI", Receiver::url("/dm")},
                     {"dataSub",
                      json{{"nrfDataSub",
                            json{{"nfStatusNotificationUri", "https://placeholder.invalid/"}}}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string dm_location = r->headers.find("location")->second;
    ASSERT_NE(dm_location.find(std::string(kDm) + "/subscriptions/"), std::string::npos);

    nf_test::SpawnedProcess nsacf(NSACF_PATH); // registers at the NRF -> NF_REGISTERED
    ASSERT_TRUE(receiver.wait_until("/dm", 40s, [](const Receiver::Note& n) {
        return carries(n.body, "NF_REGISTERED", "NSACF");
    })) << "the NSACF's registration never reached the consumer through DCCF/MFAF/NWDAF";
    {
        const auto notes = receiver.notes("/dm");
        EXPECT_EQ(notes.front().callback, "Nnwdaf_DataManagement_myNotification");
        EXPECT_EQ(notes.front().body["notifCorrId"], "dm-1");
        EXPECT_TRUE(notes.front().body.contains("notifTimestamp"));
    }

    // ---- NF_LOAD from the collected timeline: deregister the NSACF, watch the shares move ----
    const std::string nsacf_id = nf_instance_id_of(c, "NSACF");
    ASSERT_FALSE(nsacf_id.empty());
    const auto registered_at = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(3s);
    {
        const auto token = token_for(c, "nnrf-nfm", "NRF");
        auto del = request("DELETE", std::string(kNrf) + "/nnrf-nfm/v1/nf-instances/" + nsacf_id);
        del.headers.emplace("authorization", "Bearer " + token);
        r = c.send(del);
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 204) << r->body;
    }
    ASSERT_TRUE(receiver.wait_until("/dm", 30s, [&](const Receiver::Note& n) {
        for (const auto& e : n.body.value("dataNotification", json::object())
                                 .value("nrfEventNotifs", json::array())) {
            if (e.value("event", "") == "NF_DEREGISTERED" &&
                e.value("nfInstanceUri", "").find(nsacf_id) != std::string::npos) {
                return true;
            }
        }
        return false;
    })) << "the deregistration never reached the consumer";
    // Let the unregistered share grow to a clearly non-zero fraction of the observed span.
    std::this_thread::sleep_for(3s);
    r = c.send(request("GET",
                       std::string(kNwdaf) +
                           "/nnwdaf-analyticsinfo/v1/analytics?event-id=NF_LOAD&event-filter=" +
                           json{{"nfInstanceIds", json::array({nsacf_id})}}.dump()));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    {
        const auto data = json::parse(r->body);
        ASSERT_EQ(data["nfLoadLevelInfos"].size(), 1U)
            << "the deregistered NSACF must still be reported from its history: " << r->body;
        const auto& info = data["nfLoadLevelInfos"][0];
        EXPECT_EQ(info["nfInstanceId"], nsacf_id);
        EXPECT_EQ(info["nfType"], "NSACF");
        ASSERT_TRUE(info.contains("nfStatus")) << info.dump();
        EXPECT_LT(info["nfStatus"].value("statusRegistered", 100), 100) << info.dump();
        EXPECT_GE(info["nfStatus"].value("statusUnregistered", 0), 1) << info.dump();
        const auto observed_for = std::chrono::steady_clock::now() - registered_at;
        EXPECT_GT(observed_for, 5s);
    }

    // ---- history: a subscription with a timePeriod gets what was collected in it -----------
    r = c.send(request(
        "POST",
        std::string(kNwdaf) + kDm + "/subscriptions",
        json{{"notifCorrId", "dm-history"},
             {"notificURI", Receiver::url("/dm-history")},
             {"dataSub",
              json{{"nrfDataSub",
                    json{{"nfStatusNotificationUri", "https://placeholder.invalid/"}}}}},
             {"timePeriod", json{{"startTime", now_plus(-600s)}, {"stopTime", now_plus(600s)}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string history_location = r->headers.find("location")->second;
    ASSERT_TRUE(receiver.wait_until("/dm-history", 20s, [](const Receiver::Note& n) {
        return carries(n.body, "NF_REGISTERED", "NSACF");
    })) << "the historical slice did not carry the NSACF registration";

    // ---- consTrigNotif: fetch instructions, redeemed once at the fetchUri --------------------
    r = c.send(
        request("POST",
                std::string(kNwdaf) + kDm + "/subscriptions",
                json{{"notifCorrId", "dm-fetch"},
                     {"notificURI", Receiver::url("/dm-fetch")},
                     {"formatInstruct", json{{"consTrigNotif", true}}},
                     {"dataSub",
                      json{{"nrfDataSub",
                            json{{"nfStatusNotificationUri", "https://placeholder.invalid/"}}}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string fetch_location = r->headers.find("location")->second;
    nf_test::SpawnedProcess hello(HELLO_NF_PATH); // another registration to be notified about
    ASSERT_TRUE(receiver.wait_until("/dm-fetch", 40s, [](const Receiver::Note& n) {
        return n.body.contains("fetchInstruct");
    })) << "no fetch instruction arrived";
    {
        const auto n = receiver.notes("/dm-fetch").front();
        EXPECT_FALSE(n.body.contains("dataNotification"));
        const auto ids = n.body["fetchInstruct"]["fetchCorrIds"];
        ASSERT_EQ(ids.size(), 1U) << n.body.dump();
        const std::string fetch_uri = n.body["fetchInstruct"]["fetchUri"];
        EXPECT_EQ(fetch_uri, std::string(kNwdaf) + "/nwdaf-inbound/v1/fetch");
        r = c.send(request("POST", fetch_uri, ids));
        ASSERT_TRUE(r.has_value()) << r.error();
        ASSERT_EQ(r->status, 200) << r->body;
        const auto fetched = json::parse(r->body);
        EXPECT_EQ(fetched["notifCorrId"], "dm-fetch");
        EXPECT_GE(fetched["dataNotification"]["nrfEventNotifs"].size(), 1U) << r->body;
        r = c.send(request("POST", fetch_uri, ids));
        ASSERT_TRUE(r.has_value()) << r.error();
        EXPECT_EQ(r->status, 404) << r->body; // consumed once
    }

    // ---- update and unsubscribe -------------------------------------------------------------
    r = c.send(
        request("PUT",
                std::string(kNwdaf) + dm_location,
                json{{"notifCorrId", "dm-1b"},
                     {"notificURI", Receiver::url("/dm")},
                     {"dataSub",
                      json{{"nrfDataSub",
                            json{{"nfStatusNotificationUri", "https://placeholder.invalid/"}}}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 200) << r->body;
    for (const auto& loc : {dm_location, history_location, fetch_location}) {
        r = c.send(request("DELETE", std::string(kNwdaf) + loc));
        ASSERT_TRUE(r.has_value()) << r.error();
        EXPECT_EQ(r->status, 204) << r->body;
    }
    r = c.send(request("DELETE", std::string(kNwdaf) + dm_location));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 404) << r->body;
}

TEST(NwdafPhaseC, AdrfStoresNwdafCollectedDataThroughDataManagement) {
#ifdef NWDAF_C_TEST_TSAN
    GTEST_SKIP() << "the MFAF is a librdkafka process; its threads are invisible to "
                    "ThreadSanitizer -- skipped, not passed";
#endif
    const auto b = brokers();
    if (!broker_reachable(b)) {
        GTEST_SKIP() << "no Kafka broker at " << b << " -- skipped, not passed";
    }
    auto c = make_client();
    auto lab = spawn_lab(c, b);
    nf_test::SpawnedProcess adrf(ADRF_PATH);
    ASSERT_TRUE(wait_up(c, std::string(kNwdaf) + kDm + "/subscriptions", 30s))
        << "NWDAF never came up";
    ASSERT_TRUE(wait_up(c, std::string(kAdrf) + "/nadrf-datamanagement/v1/data-store-records", 30s))
        << "ADRF never came up";
    {
        auto r =
            c.send(request("GET",
                           std::string(kAdrf) +
                               "/nadrf-datamanagement/v1/data-store-records?store-trans-id=probe"));
        if (r && r->status == 500 && r->body.find("data store unreachable") != std::string::npos) {
            GTEST_SKIP() << "no Apache Doris behind the ADRF -- skipped, not passed";
        }
    }
    std::this_thread::sleep_for(2s);
    const std::string nwdaf_id = nf_instance_id_of(c, "NWDAF");
    ASSERT_FALSE(nwdaf_id.empty());

    const std::string set_id = "nwdaf-data-" + std::to_string(std::time(nullptr));
    auto r = c.send(
        request("POST",
                std::string(kAdrf) + "/nadrf-datamanagement/v1/request-storage-sub",
                json{{"dataSub",
                      json{{"nrfDataSub",
                            json{{"nfStatusNotificationUri", "https://placeholder.invalid/"}}}}},
                     {"targetNfId", nwdaf_id},
                     {"dataSetTag", json{{"dataSetId", set_id}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    const std::string trans_ref_id = json::parse(r->body).at("transRefId");

    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    const auto deadline = std::chrono::steady_clock::now() + 40s;
    json got;
    while (std::chrono::steady_clock::now() < deadline) {
        r = c.send(
            request("GET",
                    std::string(kAdrf) +
                        "/nadrf-datamanagement/v1/data-store-records?data-set-id=" + set_id));
        ASSERT_TRUE(r.has_value()) << r.error();
        if (r->status == 200) {
            const auto body = json::parse(r->body);
            bool nsacf_seen = false;
            for (const auto& e :
                 body.value("dataNotif", json::object()).value("nrfEventNotifs", json::array())) {
                nsacf_seen |= e.value("nfProfile", json::object()).value("nfType", "") == "NSACF";
            }
            if (nsacf_seen) {
                got = body;
                break;
            }
        }
        std::this_thread::sleep_for(500ms);
    }
    ASSERT_FALSE(got.is_null())
        << "the NSACF registration never reached the ADRF through NWDAF Nnwdaf_DataManagement";
    ASSERT_TRUE(got["dataSub"][0].contains("nrfDataSub")) << got.dump();

    r = c.send(request("POST",
                       std::string(kAdrf) + "/nadrf-datamanagement/v1/request-storage-sub-removal",
                       json{{"transRefId", trans_ref_id}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
    r = c.send(
        request("POST",
                std::string(kAdrf) + "/nadrf-datamanagement/v1/remove-stored-data-analytics",
                json{{"dataSetId", set_id},
                     {"timePeriod",
                      json{{"startTime", now_plus(-3600s)}, {"stopTime", now_plus(3600s)}}}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204) << r->body;
}
