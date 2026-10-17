// ADR-0487 increment 1: the NEF IRI-POI (TS 33.128 clause 7.7.6.1.2) at the REAL nef process. The
// test is the AF (TS 29.122 AsSessionWithQoS create / update / patch / revoke), the warrant is
// provisioned over real LI_X1, and the xIRIs are read off a loopback MDF2's real LI_X2 server and
// decoded with the MDF2's own codec. The PCF the NEF would ask for an app session is pointed at a
// closed port: the NEF's own response to the AF -- what the record reports -- does not depend on
// it (a failed PCF call is logged and the subscription still stands, existing NEF behaviour).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
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

constexpr const char* kTargetMsisdn = "491701230001";
constexpr const char* kOtherMsisdn = "491701230002";
constexpr std::uint16_t kX1Port = 19863;
constexpr const char* kX1Url = "https://127.0.0.1:19863/X1/NE";
constexpr const char* kNeId = "nef-poi-e2e";
constexpr const char* kQosBase = "https://127.0.0.1:7790/3gpp-as-session-with-qos/v1/af-qos-e2e";

class Inbox {
public:
    void push(li_core::Pdu v) {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(v));
        cv_.notify_all();
    }
    bool wait_for(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return items_.size() >= n; });
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
    cfg.server_cert_path = CERTS_DIR "/nef/cert.pem";
    cfg.server_key_path = CERTS_DIR "/nef/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

// config/nef.json with li_poi enabled toward the loopback MDF2, the PCF on a closed port.
std::string write_config(std::uint16_t mdf2_port) {
    std::ifstream in(NEF_CONFIG_TEMPLATE);
    json cfg = json::parse(in);
    cfg["pcf_base_url"] = "https://127.0.0.1:9";
    auto& lp = cfg["li_poi"];
    lp["enabled"] = true;
    lp["x1_bind_address"] = "127.0.0.1";
    lp["x1_port"] = kX1Port;
    lp["ne_identifier"] = kNeId;
    lp["mdf2_host"] = "127.0.0.1";
    lp["mdf2_port"] = mdf2_port;
    lp["mdf2_sni"] = "localhost";
    const std::string path = std::string(::testing::TempDir()) + "nef_li_e2e.json";
    std::ofstream(path) << cfg.dump(2);
    return path;
}

sbi_core::http2::Client make_client() {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

std::string x1_activate(const std::string& xid) {
    namespace x1 = li_core::x1;
    x1::TaskDetails task;
    task.xid = xid;
    task.targets.push_back({x1::TargetIdentifierKind::GpsiMsisdn, "gpsiMsisdn", kTargetMsisdn});
    task.delivery = x1::DeliveryType::X2Only;
    task.dids = {"33333333-3333-4333-8333-333333333333"};
    x1::Request request;
    request.header = {"admf-test",
                      kNeId,
                      "2026-10-14T00:00:00.000000Z",
                      "v1.23.1",
                      "00000000-0000-4000-8000-0000000000f2"};
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
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (const auto r = admf.send(req); r.has_value()) {
            return r->status == 200 && r->body.find("ErrorResponse") == std::string::npos;
        }
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

sbi_core::http2::ClientResponse
af_request(const std::string& method, const std::string& url, const json& body) {
    auto client = make_client();
    nf_test::attach_test_tokens(client); // ADR-0499: the NEF enforces tokens
    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    if (!body.is_null()) {
        req.headers.emplace("content-type",
                            method == "PATCH" ? "application/merge-patch+json"
                                              : "application/json");
        req.body = body.dump();
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (auto r = client.send(req); r.has_value()) {
            return *r;
        }
        std::this_thread::sleep_for(100ms);
    }
    ADD_FAILURE() << method << " " << url << " never answered";
    return {};
}

// TS 29.122 AsSessionWithQoSSubscription: notificationDestination, a flow, a QoS reference, GPSI.
json qos_subscription(const std::string& msisdn) {
    return json{{"notificationDestination", "https://127.0.0.1:9/af-notify"},
                {"flowInfo",
                 json::array({{{"flowId", 1},
                               {"flowDescriptions",
                                json::array({"permit out ip from 10.0.0.1 to 10.45.0.2"})}}})},
                {"qosReference", "qos-gold"},
                {"gpsi", std::string("msisdn-") + msisdn},
                {"ueIpv4Addr", "10.45.0.2"}};
}

std::vector<xiri::NefAfSessionWithQosProvision> records(const std::vector<li_core::Pdu>& pdus) {
    std::vector<xiri::NefAfSessionWithQosProvision> out;
    for (const auto& pdu : pdus) {
        if (const auto d = xiri::decode_xiri_payload(pdu.payload); d.has_value()) {
            if (const auto* e = std::get_if<xiri::NefAfSessionWithQosProvision>(&d->event)) {
                out.push_back(*e);
            }
        }
    }
    return out;
}

struct Lab {
    Inbox mdf2;
    li_core::X2X3Server mdf2_server{
        fake_mdf2_config(), [this](const li_core::Pdu& pdu, std::string_view) { mdf2.push(pdu); }};
};

} // namespace

TEST(LiNefEndToEnd, AfSessionWithQosCreateUpdatePatchRevokeEachReportAProvisionRecord) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port());
    ::setenv("NEF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess nef{NEF_PATH};
    ::unsetenv("NEF_CONFIG_FILE");
    ASSERT_GT(nef.pid(), 0);

    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate("f0e1e2e3-e4e5-46e7-88e9-eaebecedeeef")));

    const json sub = qos_subscription(kTargetMsisdn);
    const auto created = af_request("POST", std::string(kQosBase) + "/subscriptions", sub);
    ASSERT_EQ(created.status, 201) << created.body;
    const auto location = created.headers.find("location");
    ASSERT_NE(location, created.headers.end());
    const std::string resource = "https://127.0.0.1:7790" + location->second;

    json replaced = sub;
    replaced["qosReference"] = "qos-silver";
    ASSERT_EQ(af_request("PUT", resource, replaced).status, 200);
    const json patch{{"qosReference", "qos-bronze"}};
    ASSERT_EQ(af_request("PATCH", resource, patch).status, 200);
    ASSERT_EQ(af_request("DELETE", resource, json()).status, 204);

    ASSERT_TRUE(lab.mdf2.wait_for(4, 15s)) << "expected four NEFAFSessionWithQoSProvision xIRIs";
    const auto recs = records(lab.mdf2.take());
    ASSERT_EQ(recs.size(), 4U);
    using Op = xiri::AfSessionWithQosOpType;
    using Code = xiri::AfSessionWithQosResponseCode;
    const std::vector<std::pair<Op, Code>> expected{{Op::Post, Code::Created201},
                                                    {Op::Put, Code::Ok200},
                                                    {Op::Patch, Code::Ok200},
                                                    {Op::Delete, Code::NoContent204}};
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const auto& r = recs[i];
        EXPECT_EQ(std::get<xiri::Msisdn>(r.gpsi).digits, kTargetMsisdn);
        EXPECT_EQ(r.af_id, "af-qos-e2e");
        EXPECT_EQ(r.op_type, expected[i].first) << "record " << i;
        EXPECT_EQ(r.response_code, expected[i].second) << "record " << i;
    }
    // Table 7.7.6.1.2-1: the subscription for POST / PUT, the patch for PATCH, neither on DELETE.
    ASSERT_TRUE(recs[0].subscription.has_value());
    EXPECT_EQ(recs[0].subscription->reference,
              "TS29122_AsSessionWithQoS.yaml#/components/schemas/AsSessionWithQoSSubscription");
    EXPECT_EQ(json::parse(recs[0].subscription->value).at("qosReference"), "qos-gold");
    EXPECT_FALSE(recs[0].subscription_patch.has_value());
    ASSERT_TRUE(recs[1].subscription.has_value());
    EXPECT_EQ(json::parse(recs[1].subscription->value).at("qosReference"), "qos-silver");
    ASSERT_TRUE(recs[2].subscription_patch.has_value());
    EXPECT_EQ(
        recs[2].subscription_patch->reference,
        "TS29122_AsSessionWithQoS.yaml#/components/schemas/AsSessionWithQoSSubscriptionPatch");
    EXPECT_EQ(json::parse(recs[2].subscription_patch->value), patch);
    EXPECT_FALSE(recs[2].subscription.has_value());
    EXPECT_FALSE(recs[3].subscription.has_value());
    EXPECT_FALSE(recs[3].subscription_patch.has_value());
    lab.mdf2_server.stop();
}

TEST(LiNefEndToEnd, NonTargetGpsiIsServedButNotIntercepted) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port());
    ::setenv("NEF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess nef{NEF_PATH};
    ::unsetenv("NEF_CONFIG_FILE");

    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate("f1e1e2e3-e4e5-46e7-88e9-eaebecedeeef")));
    const auto created = af_request(
        "POST", std::string(kQosBase) + "/subscriptions", qos_subscription(kOtherMsisdn));
    EXPECT_EQ(created.status, 201);
    EXPECT_FALSE(lab.mdf2.wait_for(1, 2s)) << "a non-target subscription produced an xIRI";
    lab.mdf2_server.stop();
}
