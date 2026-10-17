// ADR-0475: the UDM's LI IRI-POI (TS 33.127 7.2.2) through the REAL udm process, a real nrf (OAuth2
// tokens, LI_X1 provisioning target) and a real udr (am-data for event 2). Every event this suite
// exercises is triggered by a direct SBI call to udm's own Nudm_UECM/Nudm_UEAU routes -- unlike the
// AMF/SMF POIs, the UDM's events need no NAS/NGAP-driven procedure, so no AMF/AUSF/PCF/SMF/UPF/gNB
// is spawned. The warrant is provisioned over real LI_X1 straight into the UDM's POI, and the
// xIRIs are read off a loopback MDF2's real LI_X2 server and decoded with the same codec the MDF2
// uses -- the same harness shape test_li_amf_e2e.cpp/test_li_smf_e2e.cpp already established.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29503_Nudm_UEAU_grp.hpp"
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

constexpr const char* kTargetImsiDigits = "999700000000001"; // seeded 5G_AKA subscriber (UDR/UDM)
constexpr std::uint16_t kX1Port = 19861;
constexpr const char* kX1Url = "https://127.0.0.1:19861/X1/NE";
constexpr const char* kNeId = "udm-poi-e2e";
constexpr const char* kXid = "d0d1d2d3-d4d5-46d7-88d9-dadbdcdddedf";

class Collector {
public:
    void operator()(const li_core::Pdu& pdu, std::string_view) {
        const std::lock_guard<std::mutex> lock(mutex_);
        received_.push_back(pdu);
        cv_.notify_all();
    }
    bool wait_for(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return received_.size() >= count; });
    }
    std::vector<li_core::Pdu> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<li_core::Pdu> received_;
};

li_core::X2X3ServerConfig fake_mdf2_config() {
    li_core::X2X3ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0; // ephemeral
    cfg.server_cert_path = CERTS_DIR "/udm/cert.pem";
    cfg.server_key_path = CERTS_DIR "/udm/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

// config/udm.json with only li_poi changed: enabled, this test's X1 port/NE id, and the loopback
// MDF2's ephemeral port. Everything else is the real default the rest of the suite runs with.
std::string write_li_enabled_udm_config(std::uint16_t mdf2_port) {
    std::ifstream in(UDM_CONFIG_TEMPLATE);
    json cfg = json::parse(in);
    auto& lp = cfg["li_poi"];
    lp["enabled"] = true;
    lp["x1_bind_address"] = "127.0.0.1";
    lp["x1_port"] = kX1Port;
    lp["ne_identifier"] = kNeId;
    lp["mdf2_host"] = "127.0.0.1";
    lp["mdf2_port"] = mdf2_port;
    lp["mdf2_sni"] = "localhost"; // the lab udm cert's SAN
    const std::string path = std::string(::testing::TempDir()) + "udm_li_e2e.json";
    std::ofstream(path) << cfg.dump(2);
    return path;
}

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

// An ActivateTask built by li_core's X1 client codec (ADR-0462) -- the same code the ADMF's LIPF
// uses -- so this test also proves the real UDM POI accepts what that codec emits.
std::string x1_activate(const std::string& xid, const std::string& txn) {
    namespace x1 = li_core::x1;
    x1::TaskDetails task;
    task.xid = xid;
    task.targets.push_back({x1::TargetIdentifierKind::SupiImsi, "supiimsi", kTargetImsiDigits});
    task.delivery = x1::DeliveryType::X2Only;
    task.dids = {"22222222-2222-4222-8222-222222222222"};
    x1::Request request;
    request.header = {"admf-test", kNeId, "2026-10-13T00:00:00.000000Z", "v1.23.1", txn};
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

bool wait_reachable(sbi_core::http2::Client& client, const std::string& url, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

std::string fetch_token(sbi_core::http2::Client& client, const std::string& scope) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=" + scope + "&targetNfType=UDM";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

template <typename Event> std::vector<Event> events_of(const std::vector<li_core::Pdu>& pdus) {
    std::vector<Event> out;
    for (const auto& pdu : pdus) {
        const auto decoded = xiri::decode_xiri_payload(pdu.payload);
        if (decoded.has_value()) {
            if (const auto* e = std::get_if<Event>(&decoded->event)) {
                out.push_back(*e);
            }
        }
    }
    return out;
}

std::string imsi_of(const std::optional<xiri::Supi>& supi) {
    return supi.has_value() && std::holds_alternative<xiri::Imsi>(*supi)
               ? std::get<xiri::Imsi>(*supi).digits
               : "";
}

} // namespace

TEST(LiUdmEndToEnd, RealUdmEmitsServingSystemCancelLocationAndAuthenticationResponseForATarget) {
    Collector mdf2;
    li_core::X2X3Server mdf2_server(fake_mdf2_config(), std::ref(mdf2));
    ASSERT_TRUE(mdf2_server.start().has_value());

    const auto udm_config = write_li_enabled_udm_config(mdf2_server.bound_port());
    ::setenv("UDM_CONFIG_FILE", udm_config.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess udm{UDM_PATH};
    ::unsetenv("UDM_CONFIG_FILE");
    nf_test::SpawnedProcess udr{UDR_PATH};
    ASSERT_GT(udm.pid(), 0);
    ASSERT_TRUE(nf_test::wait_tcp_listening(7781)) << "udr never started listening";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        200))
        << "udm never became reachable";
    const std::string token = fetch_token(client, "nudm-uecm nudm-ueau");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate(kXid, "00000000-0000-4000-8000-0000000000e1")))
        << "the UDM's LI_X1 listener never accepted the task -- li_poi not enabled?";

    const std::string ue_id = std::string("imsi-") + kTargetImsiDigits;
    const std::string reg_url =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id + "/registrations/amf-3gpp-access";

    // 1. AMF 3GPP-access registration -> UDMServingSystemMessage.
    sbi_gen::Amf3GppAccessRegistration create_data{};
    create_data.amfInstanceId = "00000000-0000-4000-8000-000000000aaa";
    create_data.deregCallbackUri = "https://example.com/dereg";
    create_data.guami.plmnId.mcc = "999";
    create_data.guami.plmnId.mnc = "70";
    create_data.guami.amfId = "ABCDEF";
    create_data.ratType = "NR";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url = reg_url;
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);

    ASSERT_TRUE(mdf2.wait_for(1, 15s)) << "no UDMServingSystemMessage";
    {
        const auto msgs = events_of<xiri::UdmServingSystemMessage>(mdf2.take());
        ASSERT_EQ(msgs.size(), 1U);
        EXPECT_EQ(imsi_of(msgs[0].supi), kTargetImsiDigits);
        EXPECT_EQ(msgs[0].serving_system_method,
                  xiri::UdmServingSystemMethod::Amf3GppAccessRegistration);
    }

    // 2. A merge-patch with purgeFlag=true -> UDMCancelLocationMessage.
    sbi_core::http2::ClientRequest patch_req;
    patch_req.method = "PATCH";
    patch_req.url = reg_url;
    patch_req.headers.emplace("content-type", "application/merge-patch+json");
    patch_req.headers.emplace("authorization", "Bearer " + token);
    patch_req.body = json{
        {"guami", json{{"plmnId", json{{"mcc", "999"}, {"mnc", "70"}}}, {"amfId", "ABCDEF"}}},
        {"purgeFlag",
         true}}.dump();
    auto patch_resp = client.send(patch_req);
    ASSERT_TRUE(patch_resp.has_value());
    ASSERT_EQ(patch_resp->status, 200);

    ASSERT_TRUE(mdf2.wait_for(2, 15s)) << "no UDMCancelLocationMessage";
    {
        const auto msgs = events_of<xiri::UdmCancelLocationMessage>(mdf2.take());
        ASSERT_EQ(msgs.size(), 1U);
        EXPECT_EQ(imsi_of(msgs[0].supi), kTargetImsiDigits);
        EXPECT_EQ(msgs[0].cancel_location_method,
                  xiri::UdmCancelLocationMethod::Amf3GppAccessDeregistration);
        ASSERT_TRUE(msgs[0].plmn_id.has_value());
        EXPECT_EQ(msgs[0].plmn_id->mcc, "999");
        EXPECT_EQ(msgs[0].plmn_id->mnc, "70");
    }

    // 3. Nudm_UEAuthentication generate-auth-data -> UDMUEAuthenticationResponse.
    sbi_gen::AuthenticationInfoRequest auth_body{};
    auth_body.servingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";
    auth_body.ausfInstanceId = "00000000-0000-4000-8000-000000000ddd";

    sbi_core::http2::ClientRequest auth_req;
    auth_req.method = "POST";
    auth_req.url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + ue_id + "/security-information/generate-auth-data";
    auth_req.headers.emplace("content-type", "application/json");
    auth_req.headers.emplace("authorization", "Bearer " + token);
    auth_req.body = json(auth_body).dump();
    auto auth_resp = client.send(auth_req);
    ASSERT_TRUE(auth_resp.has_value());
    ASSERT_EQ(auth_resp->status, 200);

    ASSERT_TRUE(mdf2.wait_for(3, 15s)) << "no UDMUEAuthenticationResponse";
    {
        const auto msgs = events_of<xiri::UdmUeAuthenticationResponse>(mdf2.take());
        ASSERT_EQ(msgs.size(), 1U);
        EXPECT_EQ(imsi_of(msgs[0].supi), kTargetImsiDigits);
        EXPECT_EQ(msgs[0].authentication_info_request.info_request_type,
                  xiri::UdmInfoRequestType::Ausf);
        EXPECT_EQ(msgs[0].authentication_info_request.auth_type,
                  xiri::PrimaryAuthenticationType::FiveGAka);
        EXPECT_EQ(msgs[0].authentication_info_request.serving_network_name.mcc, "999");
        EXPECT_EQ(msgs[0].authentication_info_request.serving_network_name.mnc, "070");
        ASSERT_EQ(msgs[0].authentication_info_request.rg_auth_ctx_supis.size(), 1U);
        EXPECT_EQ(imsi_of(msgs[0].authentication_info_request.rg_auth_ctx_supis[0]),
                  kTargetImsiDigits);
    }

    mdf2_server.stop();
}

TEST(LiUdmEndToEnd,
     RealUdmEmitsStartOfInterceptionWhenAWarrantIsActivatedOnAnAlreadyRegisteredSupi) {
    Collector mdf2;
    li_core::X2X3Server mdf2_server(fake_mdf2_config(), std::ref(mdf2));
    ASSERT_TRUE(mdf2_server.start().has_value());

    const auto udm_config = write_li_enabled_udm_config(mdf2_server.bound_port());
    ::setenv("UDM_CONFIG_FILE", udm_config.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess udm{UDM_PATH};
    ::unsetenv("UDM_CONFIG_FILE");
    nf_test::SpawnedProcess udr{UDR_PATH};
    ASSERT_GT(udm.pid(), 0);
    ASSERT_TRUE(nf_test::wait_tcp_listening(7781)) << "udr never started listening";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        200))
        << "udm never became reachable";
    const std::string token = fetch_token(client, "nudm-uecm");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string ue_id = std::string("imsi-") + kTargetImsiDigits;

    // Register FIRST, with no warrant active yet: no match, so no UDMServingSystemMessage either.
    sbi_gen::Amf3GppAccessRegistration create_data{};
    create_data.amfInstanceId = "00000000-0000-4000-8000-000000000aaa";
    create_data.deregCallbackUri = "https://example.com/dereg";
    create_data.guami.plmnId.mcc = "999";
    create_data.guami.plmnId.mnc = "70";
    create_data.guami.amfId = "ABCDEF";
    create_data.ratType = "NR";

    sbi_core::http2::ClientRequest create_req;
    create_req.method = "PUT";
    create_req.url =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + ue_id + "/registrations/amf-3gpp-access";
    create_req.headers.emplace("content-type", "application/json");
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = json(create_data).dump();
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(mdf2.take().size(), 0U) << "no warrant yet -- nothing may be delivered";

    // NOW activate the warrant: UDMStartOfInterceptionWithRegisteredTarget, once, under its XID,
    // its uDMSubscriptionDataSets coming from the real UDR am-data this SUPI is seeded with.
    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate(kXid, "00000000-0000-4000-8000-0000000000e2")));

    ASSERT_TRUE(mdf2.wait_for(1, 15s))
        << "no start-of-interception record for an already-registered target";
    const auto soi = events_of<xiri::UdmStartOfInterceptionWithRegisteredTarget>(mdf2.take());
    ASSERT_EQ(soi.size(), 1U);
    EXPECT_EQ(imsi_of(soi[0].supi), kTargetImsiDigits);
    EXPECT_EQ(soi[0].sbi_reference, "/nudm-sdm/v2/" + ue_id + "/am-data");
    EXPECT_FALSE(soi[0].sbi_value.empty());
    EXPECT_NE(soi[0].sbi_value.find('{'), std::string::npos)
        << "not a JSON object: " << soi[0].sbi_value;

    mdf2_server.stop();
}
