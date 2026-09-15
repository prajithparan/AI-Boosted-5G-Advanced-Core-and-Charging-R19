// MFAF end to end (ADR-0365): the TS 23.288 6.2.6.3.4 chain with this test playing the DCCF
// (step 5, Nmfaf_3daDataManagement_Configure), the Data Source (step 7, Nnf_EventExposure_Notify
// to the MFAF Notification Target Address the MFAF chose) and the data consumer (steps 8-10,
// Nmfaf_3caDataManagement_Notify received, Fetch issued). Real NRF, real MFAF over TLS 1.3 +
// mTLS, real Valkey, real Kafka: the notification crosses the Messaging Framework.
//
// Skipped, not passed, when no Kafka broker is reachable (the lab compose value if the env is
// unset), and under ThreadSanitizer (librdkafka's C11-thread broker threads are invisible to it;
// ADR-0364 CI note 3). The MFAF has no bus-less mode by design (ADR-0359: one path), so there is
// nothing to test without a broker.
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
#define MFAF_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MFAF_TEST_TSAN 1
#endif
#endif

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

constexpr const char* kMfafA = "https://127.0.0.1:7800";
constexpr const char* kMfafB = "https://127.0.0.1:7801";
constexpr const char* k3daRoot = "/nmfaf-3dadatamanagement/v1";
constexpr const char* kContextRoot = "/nmfaf-contextmanagement/v1";
constexpr int kReceiverPort = 19995;

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
        req.method = "POST";
        req.url = url;
        req.headers.emplace("content-type", "application/json");
        req.body = "{}";
        if (auto r = c.send(req); r && r->status != 0) {
            return true;
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

// The data consumer's notification endpoint: collects every Nmfaf_3caDataManagement_Notify.
class Receiver {
public:
    Receiver()
        : server_(ioc_,
                  "127.0.0.1",
                  kReceiverPort,
                  sbi_core::http2::TlsConfig{.cert_path = CERTS_DIR "/hello-nf/cert.pem",
                                             .key_path = CERTS_DIR "/hello-nf/key.pem",
                                             .ca_path = CERTS_DIR "/ca/ca.crt"}) {
        server_.add_route("POST", "/consumer-notify", [this](const sbi_core::http2::Request& req) {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                const auto cb = req.headers.find("3gpp-sbi-callback");
                callback_types_.push_back(cb == req.headers.end() ? "" : cb->second);
                notifications_.push_back(json::parse(req.body));
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
    std::string url() const {
        return "https://127.0.0.1:" + std::to_string(kReceiverPort) + "/consumer-notify";
    }
    bool wait_for(std::size_t count, std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, limit, [&] { return notifications_.size() >= count; });
    }
    std::vector<json> notifications() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return notifications_;
    }
    std::vector<std::string> callback_types() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return callback_types_;
    }

private:
    boost::asio::io_context ioc_;
    sbi_core::http2::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<json> notifications_;
    std::vector<std::string> callback_types_;
};

// TS 29.518 AmfEventNotification -- what a real AMF posts to the notification URI it was given.
json amf_notification(const std::string& supi) {
    return json{{"notifyCorrelationId", "amf-sub-1"},
                {"reportList",
                 json::array({json{{"type", "REGISTRATION_STATE_REPORT"},
                                   {"state", json{{"active", true}}}, // AmfEventState is mandatory
                                   {"timeStamp", "2026-09-15T00:00:00Z"},
                                   {"supi", supi},
                                   {"rmInfoList",
                                    json::array({json{{"rmState", "REGISTERED"},
                                                      {"accessType", "3GPP_ACCESS"}}})}}})}};
}

sbi_core::http2::ClientRequest post(const std::string& url, const json& body) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = url;
    req.headers.emplace("content-type", "application/json");
    req.body = body.dump();
    return req;
}

} // namespace

TEST(Mfaf, ConfigureNotifyDeliverFetchAndTransferAcrossReplicas) {
#ifdef MFAF_TEST_TSAN
    GTEST_SKIP() << "librdkafka's C11-thread broker threads are invisible to ThreadSanitizer -- "
                    "skipped, not passed";
#endif
    const auto b = brokers();
    if (!broker_reachable(b)) {
        GTEST_SKIP() << "no Kafka broker at " << b << " -- MFAF test skipped, not passed";
    }
    Receiver receiver;
    nf_test::SpawnedProcess nrf(NRF_PATH);
    setenv("MFAF_EVENT_BUS_BROKERS", b.c_str(), 1);
    // A topic per run, so a broker that outlives many test runs never replays an old record.
    const std::string topic = "mfaf.test." + std::to_string(std::time(nullptr));
    setenv("MFAF_EVENT_BUS_TOPIC", topic.c_str(), 1);
    setenv("MFAF_EVENT_BUS_GROUP_ID", ("mfaf-test-" + topic).c_str(), 1);
    nf_test::SpawnedProcess replica_a(MFAF_PATH);
    setenv("MFAF_PORT", "7801", 1);
    setenv("MFAF_METRICS_BIND_ADDRESS", "0.0.0.0:9488", 1);
    nf_test::SpawnedProcess replica_b(MFAF_PATH);
    unsetenv("MFAF_PORT");
    unsetenv("MFAF_METRICS_BIND_ADDRESS");
    unsetenv("MFAF_EVENT_BUS_BROKERS");
    unsetenv("MFAF_EVENT_BUS_TOPIC");
    unsetenv("MFAF_EVENT_BUS_GROUP_ID");
    const std::string a = kMfafA;
    const std::string bb = kMfafB;

    auto c = make_client();
    ASSERT_TRUE(wait_up(c, a + kContextRoot + "/transfer", 30s)) << "replica A never came up";
    ASSERT_TRUE(wait_up(c, bb + kContextRoot + "/transfer", 30s)) << "replica B never came up";

    // ---- step 5: the DCCF configures replica A; no mfafNotiInfo, so the MFAF chooses one -----
    json cfg{
        {"messageConfigurations",
         json::array({json{{"correId", "consumer-corr-1"}, {"notificationURI", receiver.url()}}})}};
    auto r = c.send(post(a + k3daRoot + "/configurations", cfg));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 201) << r->body;
    const auto loc_it = r->headers.find("location");
    ASSERT_NE(loc_it, r->headers.end());
    const std::string location = loc_it->second;
    const auto created = json::parse(r->body);
    ASSERT_TRUE(created["messageConfigurations"][0].contains("mfafNotiInfo")) << r->body;
    const std::string mfaf_notif_uri =
        created["messageConfigurations"][0]["mfafNotiInfo"]["mfafNotifUri"];
    const std::string mfaf_corre_id =
        created["messageConfigurations"][0]["mfafNotiInfo"]["mfafCorreId"];
    EXPECT_NE(mfaf_notif_uri.find(mfaf_corre_id), std::string::npos);

    // The oneOf is enforced: neither, and both, are 400 with a TS 29.500 cause.
    r = c.send(post(a + k3daRoot + "/configurations", json::object()));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["cause"], "MANDATORY_IE_MISSING");
    json both = cfg;
    both["mfafTransferInfo"] =
        json{{"refIds", json::array({"x"})}, {"mfafId", "00000000-0000-4000-8000-000000000001"}};
    r = c.send(post(a + k3daRoot + "/configurations", both));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400);

    // ---- step 7: the Data Source notifies the MFAF's target address -- on replica B, with the
    // URI replica A handed out. Same Valkey, so B knows the mapping; same Kafka, so whichever
    // replica's consumer owns the partition delivers.
    const std::string on_b = bb + mfaf_notif_uri.substr(a.size());
    auto src = post(on_b, amf_notification("imsi-999700000000001"));
    src.headers.emplace("3gpp-Sbi-Callback", "Namf_EventExposure_Notify");
    r = c.send(src);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 204) << r->body;

    // ---- step 8: exactly one Nmfaf_3caDataManagement_Notify, in the AMF bucket ---------------
    ASSERT_TRUE(receiver.wait_for(1, 30s))
        << "the notification never crossed the Messaging Framework";
    std::this_thread::sleep_for(2s); // long enough for a duplicate from the other replica to show
    auto notes = receiver.notifications();
    ASSERT_EQ(notes.size(), 1U) << "one inbound must produce one delivery, not one per replica";
    EXPECT_EQ(notes[0]["correId"], "consumer-corr-1");
    ASSERT_TRUE(notes[0].contains("dataAnaNotif")) << notes[0].dump();
    ASSERT_TRUE(notes[0]["dataAnaNotif"].contains("dataNotif")) << notes[0].dump();
    ASSERT_TRUE(notes[0]["dataAnaNotif"]["dataNotif"].contains("amfEventNotifs"))
        << notes[0].dump();
    EXPECT_EQ(notes[0]["dataAnaNotif"]["dataNotif"]["amfEventNotifs"][0]["reportList"][0]["supi"],
              "imsi-999700000000001");
    EXPECT_EQ(receiver.callback_types()[0], "Nmfaf_3caDataManagement_Notification");

    // ---- consumer-triggered delivery: PUT the configuration with consTrigNotif, then the next
    // inbound arrives as a FetchInstruction and the data is fetched from the OTHER replica.
    json cfg_fetch = created;
    cfg_fetch["messageConfigurations"][0]["formatInstruct"] = json{{"consTrigNotif", true}};
    sbi_core::http2::ClientRequest put = post(bb + location, cfg_fetch);
    put.method = "PUT";
    r = c.send(put);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;

    src = post(on_b, amf_notification("imsi-999700000000002"));
    src.headers.emplace("3gpp-Sbi-Callback", "Namf_EventExposure_Notify");
    r = c.send(src);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->status, 204);
    ASSERT_TRUE(receiver.wait_for(2, 30s)) << "the fetch instruction never arrived";
    notes = receiver.notifications();
    ASSERT_TRUE(notes[1].contains("fetchInstruction")) << notes[1].dump();
    const std::string fetch_uri = notes[1]["fetchInstruction"]["fetchUri"];
    const auto fetch_ids = notes[1]["fetchInstruction"]["fetchCorrIds"];
    ASSERT_EQ(fetch_ids.size(), 1U);
    // Fetch through replica A although the buffer was written by whichever replica delivered.
    r = c.send(post(a + fetch_uri.substr(fetch_uri.find("/mfaf-inbound")), fetch_ids));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)["dataNotif"]["amfEventNotifs"][0]["reportList"][0]["supi"],
              "imsi-999700000000002");
    // A second fetch of the same id finds nothing: the buffer was consumed.
    r = c.send(post(a + fetch_uri.substr(fetch_uri.find("/mfaf-inbound")), fetch_ids));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);

    // ---- an inbound the MFAF cannot classify (no header, ambiguous shape) is accepted at the
    // edge (it is on the bus) but never delivered -- counted, not forwarded in a wrong bucket.
    r = c.send(post(on_b, json{{"notifId", "smf-1"}, {"eventNotifs", json::array()}}));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 204);

    // ---- transfer: replica A hands the configuration over and forgets it ------------------
    r = c.send(post(a + kContextRoot + "/transfer", json{{"refIds", json::array({a + location})}}));
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->status, 200) << r->body;
    const auto tresp = json::parse(r->body);
    ASSERT_TRUE(tresp["configs"].contains(a + location)) << r->body;
    EXPECT_EQ(tresp["configs"][a + location]["messageConfigurations"][0]["correId"],
              "consumer-corr-1");
    // Gone on both replicas; the source's next notification is refused with the 29.500 cause
    // for a callback whose context no longer exists.
    sbi_core::http2::ClientRequest del = post(bb + location, json::object());
    del.method = "DELETE";
    r = c.send(del);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);
    r = c.send(post(on_b, amf_notification("imsi-999700000000003")));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 400);
    EXPECT_EQ(json::parse(r->body)["cause"], "RESOURCE_CONTEXT_NOT_FOUND");

    // Nothing else was delivered: the unclassifiable inbound and the post-transfer one stayed out.
    std::this_thread::sleep_for(2s);
    EXPECT_EQ(receiver.notifications().size(), 2U);
}
