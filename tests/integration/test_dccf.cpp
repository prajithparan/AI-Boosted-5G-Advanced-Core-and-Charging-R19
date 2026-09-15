// DCCF end to end (ADR-0366): TS 23.288 6.2.6.3.4 with real NRF, MFAF, NSACF, Valkey and Kafka.
// This test is the data consumer (step 1) and the notification endpoint (steps 8-10); the DCCF
// configures the MFAF (step 5) and subscribes the Data Source (step 6) on its own.
//
//   * Data Source NRF: subscribing to NF status via the DCCF, then spawning an NF, must deliver
//     the NRF's NFStatusNotify to this test through the MFAF -- Source -> MFAF -> consumer.
//   * Dedup (TS 23.288 5A.2): two consumers ask the DCCF for the same NSACF event; the NSACF
//     must see ONE subscription, and one slice admission must reach BOTH consumers.
//   * Teardown (steps 12-14): the first consumer leaves -- the NSACF subscription stays; the
//     last leaves -- the NSACF subscription and the MFAF configuration are removed, proved by a
//     further admission reaching nobody.
//   * Sources this DCCF has no producer for are refused with TS 29.574's own cause; per-UE
//     collection without checkedConsentInd is refused under the consumer-checked policy.
//
// Skipped, not passed, without a Kafka broker and under ThreadSanitizer (the MFAF is a
// librdkafka process; ADR-0364 CI note 3).
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
#define DCCF_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DCCF_TEST_TSAN 1
#endif
#endif

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr const char* kNrf = "https://127.0.0.1:7777";
constexpr const char* kDccf = "https://127.0.0.1:7803";
constexpr const char* kMfaf = "https://127.0.0.1:7800";
constexpr const char* kNsacf = "https://127.0.0.1:7797";
constexpr const char* kDataManagementRoot = "/ndccf-datamanagement/v1";
constexpr int kReceiverPort = 19994;

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

sbi_core::http2::ClientRequest post(const std::string& url, const json& body) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = url;
    req.headers.emplace("content-type", "application/json");
    req.body = body.dump();
    return req;
}

// Two consumer endpoints on one receiver, told apart by path.
class Receiver {
public:
    Receiver()
        : server_(ioc_,
                  "127.0.0.1",
                  kReceiverPort,
                  sbi_core::http2::TlsConfig{.cert_path = CERTS_DIR "/hello-nf/cert.pem",
                                             .key_path = CERTS_DIR "/hello-nf/key.pem",
                                             .ca_path = CERTS_DIR "/ca/ca.crt"}) {
        for (const char* path : {"/consumer-a", "/consumer-b"}) {
            server_.add_route("POST", path, [this, path](const sbi_core::http2::Request& req) {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    notes_.emplace_back(path, json::parse(req.body));
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
    static std::string url(const char* path) {
        return "https://127.0.0.1:" + std::to_string(kReceiverPort) + path;
    }
    std::size_t count(const char* path) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& [p, _] : notes_) {
            n += p == path;
        }
        return n;
    }
    bool wait_for(const char* path, std::size_t at_least, std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [&] {
            std::size_t n = 0;
            for (const auto& [p, _] : notes_) {
                n += p == path;
            }
            return n >= at_least;
        });
    }
    std::vector<json> bodies(const char* path) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<json> out;
        for (const auto& [p, b] : notes_) {
            if (p == path) {
                out.push_back(b);
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
    std::vector<std::pair<std::string, json>> notes_;
};

json nsacf_data_subscription(const char* consumer_path, const std::string& corr) {
    // TS 29.536 SACEventSubscription as the consumer would write it for the DCCF: the notify
    // target and nfId are the DCCF's to fill in (it points them at the MFAF).
    return json{{"dataSub",
                 json{{"nsacfDataSub",
                       json{{"event",
                             json{{"eventType", "NUM_OF_REGD_UES"},
                                  {"eventTrigger", "THRESHOLD"},
                                  {"eventFilter", json::array({json{{"sst", 2}}})},
                                  {"notifThreshold", json{{"numericValNumUes", 1}}}}},
                            {"eventNotifyUri", "https://placeholder.invalid/"},
                            {"nfId", "00000000-0000-4000-8000-0000000000dd"}}}}},
                {"dataNotifUri", Receiver::url(consumer_path)},
                {"dataNotifCorrId", corr}};
}

} // namespace

TEST(Dccf, CoordinatesCollectionThroughTheMfafDedupsAndTearsDown) {
#ifdef DCCF_TEST_TSAN
    GTEST_SKIP() << "the MFAF is a librdkafka process; its threads are invisible to "
                    "ThreadSanitizer -- skipped, not passed";
#endif
    const auto b = brokers();
    if (!broker_reachable(b)) {
        GTEST_SKIP() << "no Kafka broker at " << b << " -- DCCF test skipped, not passed";
    }
    Receiver receiver;
    nf_test::SpawnedProcess nrf(NRF_PATH);
    setenv("MFAF_EVENT_BUS_BROKERS", b.c_str(), 1);
    const std::string topic = "mfaf.dccf-test." + std::to_string(std::time(nullptr));
    setenv("MFAF_EVENT_BUS_TOPIC", topic.c_str(), 1);
    setenv("MFAF_EVENT_BUS_GROUP_ID", ("mfaf-dccf-test-" + topic).c_str(), 1);
    nf_test::SpawnedProcess mfaf(MFAF_PATH);
    unsetenv("MFAF_EVENT_BUS_BROKERS");
    unsetenv("MFAF_EVENT_BUS_TOPIC");
    unsetenv("MFAF_EVENT_BUS_GROUP_ID");
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    nf_test::SpawnedProcess dccf(DCCF_PATH);

    auto c = make_client();
    ASSERT_TRUE(wait_up(c, std::string(kMfaf) + "/nmfaf-3dadatamanagement/v1/configurations", 30s))
        << "MFAF never came up";
    ASSERT_TRUE(wait_up(c, std::string(kNsacf) + "/nnsacf-slice-ee/v1/subscriptions", 30s))
        << "NSACF never came up";
    ASSERT_TRUE(wait_up(c, std::string(kDccf) + kDataManagementRoot + "/data-subscriptions", 30s))
        << "DCCF never came up";
    std::this_thread::sleep_for(1s); // NRF registrations + tokens settle

    // ---- refusals first: a source without a producer here, and per-UE data without consent -----
    auto r = c.send(
        post(std::string(kDccf) + kDataManagementRoot + "/data-subscriptions",
             json{{"dataSub",
                   json{{"smfDataSub",
                         json{{"notifId", "x"},
                              {"notifUri", "https://placeholder.invalid/"},
                              {"eventSubs", json::array({json{{"event", "PDU_SES_REL"}}})}}}}},
                  {"dataNotifUri", Receiver::url("/consumer-a")},
                  {"dataNotifCorrId", "c"}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 400) << r->body;
    EXPECT_EQ(json::parse(r->body)["cause"], "SUBSCRIPTION_CANNOT_BE_SERVED");
    r = c.send(
        post(std::string(kDccf) + kDataManagementRoot + "/data-subscriptions",
             json{{"dataSub",
                   json{{"amfDataSub",
                         json{{"eventList", json::array({json{{"type", "LOCATION_REPORT"}}})},
                              {"eventNotifyUri", "https://x/"},
                              {"notifyCorrelationId", "y"},
                              {"nfId", "00000000-0000-4000-8000-0000000000dd"},
                              {"supi", "imsi-999700000000001"}}}}},
                  {"dataNotifUri", Receiver::url("/consumer-a")},
                  {"dataNotifCorrId", "c"}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 403) << r->body;
    EXPECT_EQ(json::parse(r->body)["cause"], "USER_CONSENT_NOT_GRANTED");

    // ---- NRF as Data Source: subscribe to NF status through the DCCF, then make an NF appear ---
    r = c.send(post(std::string(kDccf) + kDataManagementRoot + "/data-subscriptions",
                    json{{"dataSub",
                          json{{"nrfDataSub",
                                json{{"nfStatusNotificationUri", "https://placeholder.invalid/"},
                                     {"subscrCond", json{{"nfType", "NWDAF"}}}}}}},
                         {"dataNotifUri", Receiver::url("/consumer-a")},
                         {"dataNotifCorrId", "nrf-corr-a"}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string nrf_sub_location = r->headers.find("location")->second;
    EXPECT_NE(nrf_sub_location.find("/data-subscriptions/"), std::string::npos);

    nf_test::SpawnedProcess nwdaf(NWDAF_PATH); // registers with the NRF -> NF_REGISTERED
    ASSERT_TRUE(receiver.wait_for("/consumer-a", 1, 40s))
        << "the NRF's NFStatusNotify never reached the consumer through the MFAF";
    {
        const auto note = receiver.bodies("/consumer-a")[0];
        EXPECT_EQ(note["correId"], "nrf-corr-a");
        ASSERT_TRUE(note.contains("dataAnaNotif")) << note.dump();
        ASSERT_TRUE(note["dataAnaNotif"]["dataNotif"].contains("nrfEventNotifs")) << note.dump();
        EXPECT_EQ(note["dataAnaNotif"]["dataNotif"]["nrfEventNotifs"][0]["event"], "NF_REGISTERED");
    }

    // ---- NSACF: two consumers, one source subscription (5A.2 dedup) -------------------------
    r = c.send(post(std::string(kDccf) + kDataManagementRoot + "/data-subscriptions",
                    nsacf_data_subscription("/consumer-a", "nsacf-corr-a")));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string nsacf_sub_a = r->headers.find("location")->second;
    r = c.send(post(std::string(kDccf) + kDataManagementRoot + "/data-subscriptions",
                    nsacf_data_subscription("/consumer-b", "nsacf-corr-b")));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const std::string nsacf_sub_b = r->headers.find("location")->second;

    const auto admit = [&](const std::string& supi) {
        auto req =
            post(std::string(kNsacf) + "/nnsacf-nsac/v1/slices/ues",
                 json{{"ueACRequestInfo",
                       json::array({json{{"supi", supi},
                                         {"anType", "3GPP_ACCESS"},
                                         {"acuOperationList",
                                          json::array({json{{"updateFlag", "INCREASE"},
                                                            {"snssai", json{{"sst", 2}}}}})}}})},
                      {"nfId", "00000000-0000-4000-8000-0000000000aa"}});
        req.headers.emplace("authorization", "Bearer " + token_for(c, "nnsacf-nsac", "NSACF"));
        auto resp = c.send(req);
        return resp ? resp->status : -1;
    };
    ASSERT_EQ(admit("imsi-999700000000801"), 204);
    ASSERT_TRUE(receiver.wait_for("/consumer-a", 2, 30s))
        << "consumer A never got the NSACF report";
    ASSERT_TRUE(receiver.wait_for("/consumer-b", 1, 30s))
        << "consumer B never got the NSACF report";
    {
        const auto a = receiver.bodies("/consumer-a")[1];
        const auto bb = receiver.bodies("/consumer-b")[0];
        EXPECT_EQ(a["correId"], "nsacf-corr-a");
        EXPECT_EQ(bb["correId"], "nsacf-corr-b");
        ASSERT_TRUE(a["dataAnaNotif"]["dataNotif"].contains("nsacfEventNotifs")) << a.dump();
        EXPECT_EQ(a["dataAnaNotif"]["dataNotif"]["nsacfEventNotifs"][0]["report"]["eventType"],
                  "NUM_OF_REGD_UES");
    }

    // ---- teardown: A leaves, B still served; B leaves, nobody served -----------------------
    sbi_core::http2::ClientRequest del;
    del.method = "DELETE";
    del.url = std::string(kDccf) + nsacf_sub_a;
    r = c.send(del);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204);
    ASSERT_EQ(admit("imsi-999700000000802"), 204);
    ASSERT_TRUE(receiver.wait_for("/consumer-b", 2, 30s))
        << "consumer B lost its data when A unsubscribed";
    std::this_thread::sleep_for(2s);
    EXPECT_EQ(receiver.count("/consumer-a"), 2U)
        << "consumer A was still delivered to after unsubscribing";

    del.url = std::string(kDccf) + nsacf_sub_b;
    r = c.send(del);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->status, 204);
    r = c.send(del); // a second DELETE of the same id is a 404, never a silent 204
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);
    ASSERT_EQ(admit("imsi-999700000000803"), 204);
    std::this_thread::sleep_for(3s);
    EXPECT_EQ(receiver.count("/consumer-b"), 2U)
        << "the NSACF subscription outlived its last consumer";

    // The NRF collection is still alive (its consumer never left): one more NF registering
    // must still be reported. Then clean it up.
    setenv("NWDAF_PORT", "7799", 1);
    setenv("NWDAF_METRICS_BIND_ADDRESS", "0.0.0.0:9486", 1);
    nf_test::SpawnedProcess nwdaf2(NWDAF_PATH);
    unsetenv("NWDAF_PORT");
    unsetenv("NWDAF_METRICS_BIND_ADDRESS");
    ASSERT_TRUE(receiver.wait_for("/consumer-a", 3, 40s))
        << "the NRF collection stopped delivering";
    del.url = std::string(kDccf) + nrf_sub_location;
    r = c.send(del);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 204);
}
