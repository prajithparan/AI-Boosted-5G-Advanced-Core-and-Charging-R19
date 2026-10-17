// Real, automated end-to-end test for UDR's onDataChange webhook delivery (docs/DECISIONS.md
// ADR-0171 through ADR-0178). Every prior batch in that series was live-verified manually (curl +
// a standalone HTTPS receiver script, disclosed as such in each ADR) -- this closes that real,
// previously-disclosed testing gap by driving the same flow inside ctest.
//
// Spawns nrf and udr as real, separate OS processes (same pattern as test_udr_context_data.cpp),
// plus a real subscriber's callback endpoint: an in-process sbi_core::http2::Server -- the exact
// same TLS 1.3 + mTLS server implementation every NF in this project uses, not a stub or mock --
// running on its own io_context thread. Subscribes via a real POST subs-to-notify, mutates the
// watched resource over real TLS 1.3 + mTLS HTTP/2, and asserts the receiver actually got a
// correctly-shaped DataChangeNotify delivered over the wire.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// TS29505_Subscription_Data's own types now live in TS26510_CommonData_grp.hpp -- see
// nfs/chf/src/stores.hpp's own comment (ADR-0072).
#include "TS26510_CommonData_grp.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

// RAII wrapper for the in-process receiver's io_context thread -- same early-return hazard as
// nf_test::SpawnedProcess (spawn_guard.hpp): a still-joinable std::thread destructing without
// join() calls std::terminate(), which is exactly what happened before this wrapper existed.
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

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nudr-dr&"
        "targetNfType=UDR";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

} // namespace

TEST(UdrIntegration, OnDataChangeWebhookDeliveredOnPutPatchDelete) {
    // Real receiver: an actual sbi_core::http2::Server (TLS 1.3 + mTLS, requires and verifies
    // udr's own client certificate against the shared CA, exactly like every other server in this
    // project) standing in for a real Nudr_DataRepository subscriber's callback endpoint.
    boost::asio::io_context receiver_ioc;
    sbi_core::http2::TlsConfig receiver_tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Server receiver(receiver_ioc, "127.0.0.1", 19999, receiver_tls);

    std::mutex received_mutex;
    std::vector<json> received;
    receiver.add_route(
        "POST", "/callback", [&received_mutex, &received](const sbi_core::http2::Request& req) {
            std::lock_guard<std::mutex> lock(received_mutex);
            received.push_back(json::parse(req.body));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    receiver.start();
    IoContextThread receiver_thread(receiver_ioc);

    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    // smf-registrations (not amf-3gpp-access): the resource under test needs real PUT+PATCH+
    // DELETE all on the same document to exercise all three real ChangeItem shapes
    // (change_replace/change_from_json_patch/change_remove) in one flow -- confirmed by direct
    // read of nfs/udr/src/main.cpp that amf-3gpp-access genuinely has no DELETE operation at all
    // (GET+PUT+PATCH only), so it can't cover that third shape.
    // Real ue_id/pduSessionId collision found while writing this test: a run that fails before
    // reaching its own DELETE leaves the row behind in the real, persistent PostgreSQL backing
    // store (by design, ADR-0079), so a fixed literal ID collides with that leftover row on the
    // next run (PUT then returns 204-already-exists, not 201). Same real precedent already
    // established in test_n28_spending_limit.cpp: embed getpid() so each run gets a fresh row
    // regardless of how the previous run ended.
    const std::string ue_id = "imsi-99970000" + std::to_string(getpid());
    const std::string collection_path =
        "/nudr-dr/v2/subscription-data/" + ue_id + "/context-data/smf-registrations";
    const std::string resource_path = collection_path + "/7";
    const std::string resource_url = "https://127.0.0.1:7781" + resource_path;
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7781" + collection_path, 200))
        << "udr never became reachable";

    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // Real POST subs-to-notify subscription. monitoredResourceUris' own real match semantics
    // (nfs/udr/src/main.cpp's notify_subscribers()) require the subscription's own URI to CONTAIN
    // the target resource's real resolved path as a substring -- the full path is used here, not
    // a shorthand, matching that real, disclosed matching rule.
    sbi_core::http2::ClientRequest sub_req;
    sub_req.method = "POST";
    sub_req.url = "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/subs-to-notify";
    sub_req.headers.emplace("content-type", "application/json");
    sub_req.headers.emplace("authorization", "Bearer " + token);
    sub_req.body =
        json{
            {"ueId", ue_id},
            {"callbackReference", "https://127.0.0.1:19999/callback"},
            {"monitoredResourceUris", json::array({resource_path})},
        }
            .dump();
    auto sub_resp = client.send(sub_req);
    ASSERT_TRUE(sub_resp.has_value());
    ASSERT_EQ(sub_resp->status, 201);

    sbi_gen::SmfRegistration create_data{};
    create_data.smfInstanceId = "00000000-0000-4000-8000-000000000ccc";
    create_data.pduSessionId = 7;
    create_data.singleNssai.sst = 1;
    create_data.plmnId.mcc = "999";
    create_data.plmnId.mnc = "70";

    sbi_core::http2::ClientRequest put_req;
    put_req.method = "PUT";
    put_req.url = resource_url;
    put_req.headers.emplace("content-type", "application/json");
    put_req.headers.emplace("authorization", "Bearer " + token);
    put_req.body = json(create_data).dump();
    auto put_resp = client.send(put_req);
    ASSERT_TRUE(put_resp.has_value());
    ASSERT_EQ(put_resp->status, 201);

    // Real delivery is synchronous within the PUT handler itself (notify_subscribers() is called
    // before the handler returns, docs/DECISIONS.md ADR-0171) -- by the time the PUT response has
    // come back to this client, the POST to the receiver has already completed. No
    // sleep-and-hope polling needed.
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        ASSERT_EQ(received.size(), 1U) << "onDataChange webhook was not delivered for PUT";
        const auto& notify = received.front();
        EXPECT_EQ(notify.at("ueId").get<std::string>(), ue_id);
        ASSERT_TRUE(notify.contains("notifyItems"));
        ASSERT_EQ(notify.at("notifyItems").size(), 1U);
        const auto& item = notify.at("notifyItems").at(0);
        EXPECT_EQ(item.at("resourceId").get<std::string>(), resource_path);
        ASSERT_TRUE(item.contains("changes"));
        ASSERT_EQ(item.at("changes").size(), 1U);
        const auto& change = item.at("changes").at(0);
        EXPECT_EQ(change.at("op").get<std::string>(), "REPLACE");
        EXPECT_EQ(change.at("path").get<std::string>(), "/");
        EXPECT_EQ(change.at("newValue").at("smfInstanceId").get<std::string>(),
                  create_data.smfInstanceId);
    }

    // Real RFC 6902 PATCH -> real change_from_json_patch() shape, forwarding the submitted op.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = resource_url;
    patch_req.headers.emplace("content-type", "application/json-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body =
        json::array({json{{"op", "add"}, {"path", "/smfSetId"}, {"value", "set1"}}}).dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    ASSERT_EQ(patch_resp->status, 204);

    {
        std::lock_guard<std::mutex> lock(received_mutex);
        ASSERT_EQ(received.size(), 2U) << "onDataChange webhook was not delivered for PATCH";
        const auto& notify = received.at(1);
        ASSERT_TRUE(notify.contains("notifyItems"));
        const auto& item = notify.at("notifyItems").at(0);
        EXPECT_EQ(item.at("resourceId").get<std::string>(), resource_path);
        ASSERT_TRUE(item.contains("changes"));
        ASSERT_EQ(item.at("changes").size(), 1U);
        const auto& change = item.at("changes").at(0);
        EXPECT_EQ(change.at("op").get<std::string>(), "ADD");
        EXPECT_EQ(change.at("path").get<std::string>(), "/smfSetId");
        EXPECT_EQ(change.at("newValue").get<std::string>(), "set1");
    }

    // Real DELETE -> real change_remove() shape, a third, independent delivery on the same
    // subscription.
    sbi_core::http2::ClientRequest del_req;
    del_req.method = "DELETE";
    del_req.url = resource_url;
    del_req.headers.emplace("authorization", "Bearer " + token);
    auto del_resp = client.send(del_req);
    ASSERT_TRUE(del_resp.has_value());
    ASSERT_EQ(del_resp->status, 204);

    {
        std::lock_guard<std::mutex> lock(received_mutex);
        ASSERT_EQ(received.size(), 3U) << "onDataChange webhook was not delivered for DELETE";
        const auto& notify = received.at(2);
        ASSERT_TRUE(notify.contains("notifyItems"));
        const auto& item = notify.at("notifyItems").at(0);
        EXPECT_EQ(item.at("resourceId").get<std::string>(), resource_path);
        ASSERT_TRUE(item.contains("changes"));
        ASSERT_EQ(item.at("changes").size(), 1U);
        const auto& change = item.at("changes").at(0);
        EXPECT_EQ(change.at("op").get<std::string>(), "REMOVE");
        EXPECT_EQ(change.at("path").get<std::string>(), "/");
    }
}
