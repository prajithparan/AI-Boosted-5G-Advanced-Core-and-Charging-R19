// ADR-0498: AF requested QoS for a UE through the TSCTSF, end to end through real processes --
// NRF, UDR (identity data: GPSI -> SUPI), UDM (Nudm_SDM id-translation-result), PCF (SM policy ->
// BSF binding registration; Npcf_PolicyAuthorization), BSF, TSCTSF, NEF. The test is the SMF (it
// creates the SM policy for a PDU session) and the AF (TS 29.522 AsSessionWithQoS with a GPSI and
// no UE address, which TS 29.522 4.4.9.3 routes through the TSCTSF).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>

#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* kSupi = "imsi-999700000000001";
constexpr const char* kGpsi = "msisdn-491701239901";
constexpr const char* kUeIp = "10.45.0.9";
constexpr const char* kPcf = "https://127.0.0.1:7783";
constexpr const char* kTsctsf = "https://127.0.0.1:7825/ntsctsf-qos-tscai/v1";

sbi_core::http2::Client make_client() {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

std::string token(sbi_core::http2::Client& c, const std::string& scope, const std::string& nf) {
    for (int i = 0; i < 100; ++i) {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7777/oauth2/token";
        req.headers.emplace("content-type", "application/x-www-form-urlencoded");
        req.body = "grant_type=client_credentials&nfInstanceId=" +
                   std::string(nf_test::kTestAfInstanceId) + "&scope=" + scope +
                   "&targetNfType=" + nf;
        if (auto r = nf_test::send_token_request(req); r && r->status == 200) {
            return json::parse(r->body).at("access_token").get<std::string>();
        }
        std::this_thread::sleep_for(100ms);
    }
    return "";
}

sbi_core::http2::ClientResponse send(sbi_core::http2::Client& c,
                                     const std::string& method,
                                     const std::string& url,
                                     const std::string& bearer,
                                     const std::string& content_type,
                                     const std::string& body) {
    sbi_core::http2::ClientRequest req;
    req.method = method;
    req.url = url;
    if (!bearer.empty()) {
        req.headers.emplace("authorization", "Bearer " + bearer);
    }
    if (!body.empty()) {
        req.headers.emplace("content-type", content_type);
        req.body = body;
    }
    for (int i = 0; i < 100; ++i) {
        if (auto r = c.send(req)) {
            return *r;
        }
        std::this_thread::sleep_for(100ms);
    }
    ADD_FAILURE() << method << " " << url << " never answered";
    return {};
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// The PCF AF-session ids the TSCTSF logged ("AF session <id> at PCF <url>"), in order.
std::vector<std::string> af_sessions(const std::string& log) {
    std::vector<std::string> out;
    static const std::regex kLine(R"(AF session (\S+) at PCF)");
    for (auto it = std::sregex_iterator(log.begin(), log.end(), kLine);
         it != std::sregex_iterator();
         ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

} // namespace

TEST(TsctsfEndToEnd, GpsiOnlyQosRequestReachesThePcfOfThePduSessionThroughTheTsctsf) {
    const std::string tsctsf_log = std::string(::testing::TempDir()) + "tsctsf_e2e.log";
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(200ms);
    nf_test::SpawnedProcess udr(UDR_PATH);
    nf_test::SpawnedProcess udm(UDM_PATH);
    nf_test::SpawnedProcess bsf(BSF_PATH);
    nf_test::SpawnedProcess pcf(PCF_PATH);
    nf_test::SpawnedProcess tsctsf(TSCTSF_PATH, tsctsf_log.c_str());
    nf_test::SpawnedProcess nef(NEF_PATH);
    for (const int p : {7781, 7780, 7789, 7783, 7825, 7790}) {
        nf_test::wait_tcp_listening(p);
    }
    auto c = make_client();
    nf_test::attach_test_tokens(c); // ADR-0499: the TSCTSF enforces tokens

    // Provisioning: the GPSI's identity data (GPSI -> SUPI) at the UDR.
    const auto udr_token = token(c, "nudr-dr", "UDR");
    const auto ident = send(
        c,
        "PATCH",
        std::string("https://127.0.0.1:7781/nudr-dr/v2/subscription-data/") + kGpsi +
            "/identity-data",
        udr_token,
        "application/json-patch+json",
        json::array({json{{"op", "add"}, {"path", "/supiList"}, {"value", json::array({kSupi})}}})
            .dump());
    ASSERT_TRUE(ident.status == 200 || ident.status == 204) << ident.status << " " << ident.body;

    // The test as the SMF: an SM policy for an IPv4 PDU session -> the PCF registers the binding.
    const auto sm_token = token(c, "npcf-smpolicycontrol", "PCF");
    const json sm_ctx{{"supi", kSupi},
                      {"pduSessionId", 5},
                      {"pduSessionType", "IPV4"},
                      {"dnn", "internet"},
                      {"sliceInfo", json{{"sst", 1}}},
                      {"ipv4Address", kUeIp},
                      {"notificationUri", "https://127.0.0.1:9/sm-notify"}};
    const auto sm = send(c,
                         "POST",
                         std::string(kPcf) + "/npcf-smpolicycontrol/v1/sm-policies",
                         sm_token,
                         "application/json",
                         sm_ctx.dump());
    ASSERT_EQ(sm.status, 201) << sm.body;
    const std::string sm_policy = sm.headers.find("location")->second;

    const auto bsf_token = token(c, "nbsf-management", "BSF");
    sbi_core::http2::ClientResponse binding;
    for (int i = 0; i < 30; ++i) {
        binding = send(
            c,
            "GET",
            std::string("https://127.0.0.1:7789/nbsf-management/v1/pcfBindings?ipv4Addr=") + kUeIp,
            bsf_token,
            "",
            "");
        if (binding.status == 200) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_EQ(binding.status, 200) << "the PCF did not register the PDU session binding";
    EXPECT_EQ(json::parse(binding.body).value("supi", ""), kSupi);

    // The AF: AsSessionWithQoS with a GPSI and no UE address, via the NEF.
    const json qos{
        {"notificationDestination", "https://127.0.0.1:9/af-notify"},
        {"gpsi", kGpsi},
        {"dnn", "internet"},
        {"snssai", json{{"sst", 1}}},
        {"flowInfo",
         json::array({json{
             {"flowId", 1},
             {"flowDescriptions", json::array({"permit out ip from 10.0.0.1 to 10.45.0.9"})}}})},
        {"qosReference", "qos-gold"}};
    const auto created =
        send(c,
             "POST",
             "https://127.0.0.1:7790/3gpp-as-session-with-qos/v1/af-tsc/subscriptions",
             "",
             "application/json",
             qos.dump());
    ASSERT_EQ(created.status, 201) << created.body;

    // The TSCTSF created exactly one AF session; the PCF holds it, for this UE, with the AF's flow.
    std::vector<std::string> ids;
    for (int i = 0; i < 30 && ids.empty(); ++i) {
        ids = af_sessions(read_file(tsctsf_log));
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_EQ(ids.size(), 1U) << read_file(tsctsf_log);
    const auto pa_token = token(c, "npcf-policyauthorization", "PCF");
    const auto app = send(c,
                          "GET",
                          std::string(kPcf) + "/npcf-policyauthorization/v1/app-sessions/" + ids[0],
                          pa_token,
                          "",
                          "");
    ASSERT_EQ(app.status, 200) << app.body;
    const auto asc = json::parse(app.body).at("ascReqData");
    EXPECT_EQ(asc.value("ueIpv4", ""), kUeIp);
    EXPECT_EQ(asc.value("supi", ""), kSupi);
    EXPECT_EQ(asc.value("gpsi", ""), kGpsi);
    EXPECT_EQ(asc.at("medComponents").at("1").value("qosReference", ""), "qos-gold");

    // Revoke at the NEF -> the TSCTSF deletes the AF session at the PCF.
    const auto location = created.headers.find("location")->second;
    EXPECT_EQ(send(c, "DELETE", "https://127.0.0.1:7790" + location, "", "", "").status, 204);
    EXPECT_EQ(send(c,
                   "GET",
                   std::string(kPcf) + "/npcf-policyauthorization/v1/app-sessions/" + ids[0],
                   pa_token,
                   "",
                   "")
                  .status,
              404);

    // The TSCTSF directly: by UE address (no GMEC needed) -> 201; GPSI without GMEC -> 400.
    const json by_ip{{"afId", "af-tsc"},
                     {"ueIpAddr", json{{"ipv4Addr", kUeIp}}},
                     {"notifUri", "https://127.0.0.1:9/n"},
                     {"qosReference", "qos-silver"}};
    const auto ip = send(c,
                         "POST",
                         std::string(kTsctsf) + "/tsc-app-sessions",
                         "",
                         "application/json",
                         by_ip.dump());
    ASSERT_EQ(ip.status, 201) << ip.body;
    // _Update (5.3.2.3): a new QoS reference reaches the PCF's AF session (review: was untested).
    std::vector<std::string> after;
    for (int i = 0; i < 30 && after.size() < 2; ++i) {
        after = af_sessions(read_file(tsctsf_log));
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_EQ(after.size(), 2U);
    const auto ip_ctx = std::string("https://127.0.0.1:7825") + ip.headers.find("location")->second;
    const auto patched = send(c,
                              "PATCH",
                              ip_ctx,
                              "",
                              "application/merge-patch+json",
                              json{{"qosReference", "qos-bronze"}}.dump());
    EXPECT_EQ(patched.status, 200) << patched.body;
    const auto app2 =
        send(c,
             "GET",
             std::string(kPcf) + "/npcf-policyauthorization/v1/app-sessions/" + after[1],
             pa_token,
             "",
             "");
    ASSERT_EQ(app2.status, 200) << app2.body;
    EXPECT_EQ(json::parse(app2.body)
                  .at("ascReqData")
                  .at("medComponents")
                  .at("1")
                  .value("qosReference", ""),
              "qos-bronze");
    // qosReference is required (TS29565 TscAppSessionContextData; review fix).
    json no_qos = by_ip;
    no_qos.erase("qosReference");
    EXPECT_EQ(send(c,
                   "POST",
                   std::string(kTsctsf) + "/tsc-app-sessions",
                   "",
                   "application/json",
                   no_qos.dump())
                  .status,
              400);
    json no_gmec = by_ip;
    no_gmec.erase("ueIpAddr");
    no_gmec["ueId"] = kGpsi;
    EXPECT_EQ(send(c,
                   "POST",
                   std::string(kTsctsf) + "/tsc-app-sessions",
                   "",
                   "application/json",
                   no_gmec.dump())
                  .status,
              400);

    // The PDU session ends (SM policy deleted -> binding deregistered): no PDU session for the UE.
    EXPECT_EQ(send(c,
                   "POST",
                   std::string(kPcf) + sm_policy + "/delete",
                   sm_token,
                   "application/json",
                   "{}")
                  .status,
              204);
    json gmec = no_gmec;
    gmec["suppFeat"] = "20";
    const auto gone = send(
        c, "POST", std::string(kTsctsf) + "/tsc-app-sessions", "", "application/json", gmec.dump());
    EXPECT_EQ(gone.status, 500) << gone.body;
    EXPECT_EQ(json::parse(gone.body).value("cause", ""), "PDU_SESSION_NOT_AVAILABLE");
}
