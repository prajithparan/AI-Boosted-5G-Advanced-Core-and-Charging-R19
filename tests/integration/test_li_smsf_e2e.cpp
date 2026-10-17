// ADR-0486: MO and MT SMS over NAS at the REAL smsf process (TS 23.502 clauses 4.13.3.3 and
// 4.13.3.6) and its LI IRI-POI (TS 33.128 clause 6.2.5.3), with a real nrf (OAuth2). The AMF side
// is a loopback HTTP/2 stand-in that records Namf_Communication_N1N2MessageTransfer calls and
// answers Namf_MT_EnableUEReachability REACHABLE; this suite drives Nsmsf_SMService_UplinkSMS
// (`sendsms`) directly, the way the AMF does (the real AMF's relay is covered by
// tests/integration/test_amf_sms_registration.cpp). The warrant is provisioned over real LI_X1 and
// the xIRIs are read off a loopback MDF2's real LI_X2 server, decoded with the MDF2's own codec.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/multipart.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
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
#include "sms_core/sms.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;
namespace xiri = li_core::xiri;
using sms_core::Bytes;

constexpr const char* kTargetImsi = "999700000000001";
constexpr const char* kOtherImsi = "999700000000002";
constexpr std::uint16_t kX1Port = 19862;
constexpr const char* kX1Url = "https://127.0.0.1:19862/X1/NE";
constexpr const char* kNeId = "smsf-poi-e2e";
constexpr const char* kSmsfBase = "https://127.0.0.1:7793/nsmsf-sms/v2";

// The widely published SMS-SUBMIT example: TP-DA "46708251358", TP-UD "hellohello" (GSM 7 bit).
const Bytes kSubmit = {0x11, 0x00, 0x0B, 0x91, 0x64, 0x07, 0x28, 0x15, 0x53, 0xF8, 0x00, 0x00,
                       0xAA, 0x0A, 0xE8, 0x32, 0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};
const Bytes kTpDaField = {0x0B, 0x91, 0x64, 0x07, 0x28, 0x15, 0x53, 0xF8};

template <typename T> class Inbox {
public:
    void push(T v) {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(std::move(v));
        cv_.notify_all();
    }
    bool wait_for(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return items_.size() >= n; });
    }
    std::vector<T> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<T> items_;
};

struct N1n2Call {
    std::string ue_context_id;
    json body;
    Bytes n1;
};

// Loopback stand-in for the AMF's Namf_Communication n1-n2-messages resource.
class FakeAmf {
public:
    FakeAmf()
        : server_(ioc_,
                  "127.0.0.1",
                  0,
                  sbi_core::http2::TlsConfig{
                      .cert_path = CERTS_DIR "/amf/cert.pem",
                      .key_path = CERTS_DIR "/amf/key.pem",
                      .ca_path = CERTS_DIR "/ca/ca.crt",
                  }) {
        server_.add_route("POST",
                          "/namf-comm/v1/ue-contexts/{ueContextId}/n1-n2-messages",
                          [this](const sbi_core::http2::Request& req) {
                              N1n2Call call;
                              call.ue_context_id = req.path_params.at("ueContextId");
                              auto parts = sbi_core::multipart::parse(
                                  req.headers.find("content-type")->second, req.body);
                              if (parts && !parts->empty()) {
                                  call.body = json::parse((*parts)[0].body);
                                  for (const auto& p : *parts) {
                                      if (p.content_id && *p.content_id == "n1Message") {
                                          call.n1.assign(p.body.begin(), p.body.end());
                                      }
                                  }
                              }
                              calls.push(std::move(call));
                              return sbi_core::http2::Response::json(
                                  200, R"({"cause":"N1_N2_TRANSFER_INITIATED"})");
                          });
        server_.add_route("PUT",
                          "/namf-mt/v1/ue-contexts/{ueContextId}/ue-reachind",
                          [](const sbi_core::http2::Request&) {
                              return sbi_core::http2::Response::json(
                                  200, R"({"reachability":"REACHABLE"})");
                          });
        server_.start();
        thread_ = std::thread([this] { ioc_.run(); });
    }
    ~FakeAmf() {
        guard_.reset();
        ioc_.stop();
        thread_.join();
    }
    unsigned short port() const { return server_.local_port(); }
    Inbox<N1n2Call> calls;

private:
    boost::asio::io_context ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_{
        boost::asio::make_work_guard(ioc_)};
    sbi_core::http2::Server server_;
    std::thread thread_;
};

li_core::X2X3ServerConfig fake_mdf2_config() {
    li_core::X2X3ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;
    cfg.server_cert_path = CERTS_DIR "/smsf/cert.pem";
    cfg.server_key_path = CERTS_DIR "/smsf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

// config/smsf.json with li_poi enabled toward the loopback MDF2 and amf_base_url at the stand-in.
std::string write_config(std::uint16_t mdf2_port, unsigned short amf_port) {
    std::ifstream in(SMSF_CONFIG_TEMPLATE);
    json cfg = json::parse(in);
    cfg["amf_base_url"] = "https://127.0.0.1:" + std::to_string(amf_port);
    auto& lp = cfg["li_poi"];
    lp["enabled"] = true;
    lp["x1_bind_address"] = "127.0.0.1";
    lp["x1_port"] = kX1Port;
    lp["ne_identifier"] = kNeId;
    lp["mdf2_host"] = "127.0.0.1";
    lp["mdf2_port"] = mdf2_port;
    lp["mdf2_sni"] = "localhost";
    const std::string path = std::string(::testing::TempDir()) + "smsf_li_e2e.json";
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

std::string x1_activate(const std::string& xid, bool truncate) {
    namespace x1 = li_core::x1;
    x1::TaskDetails task;
    task.xid = xid;
    task.targets.push_back({x1::TargetIdentifierKind::SupiImsi, "supiimsi", kTargetImsi});
    task.delivery = x1::DeliveryType::X2Only;
    task.dids = {"22222222-2222-4222-8222-222222222222"};
    task.truncate_tp_user_data = truncate;
    x1::Request request;
    request.header = {"admf-test",
                      kNeId,
                      "2026-10-14T00:00:00.000000Z",
                      "v1.23.1",
                      "00000000-0000-4000-8000-0000000000f1"};
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

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nsmsf-sms"
        "&targetNfType=SMSF";
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (auto resp = nf_test::send_token_request(req); resp && resp->status == 200) {
            return json::parse(resp->body).at("access_token").get<std::string>();
        }
        std::this_thread::sleep_for(100ms);
    }
    return "";
}

const json kUeLocation = {
    {"nrLocation",
     {{"tai", {{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"tac", "000001"}}},
      {"ncgi", {{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"nrCellId", "000000010"}}}}}};

// Nsmsf_SMService_UplinkSMS as the AMF sends it: SmsRecordData (with ueLocation) + the SMS payload.
void send_uplink(sbi_core::http2::Client& client,
                 const std::string& token,
                 const std::string& supi,
                 const Bytes& payload) {
    sbi_core::multipart::Part json_part;
    json_part.content_type = "application/json";
    json_part.body = json{
        {"smsRecordId", "rec-1"},
        {"smsPayload", {{"contentId", "sms"}}},
        {"ueLocation",
         kUeLocation}}.dump();
    sbi_core::multipart::Part bin_part;
    bin_part.content_type = "application/vnd.3gpp.sms";
    bin_part.content_id = "sms";
    bin_part.body.assign(payload.begin(), payload.end());
    const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

    sbi_core::http2::ClientRequest post;
    post.method = "POST";
    post.url = std::string(kSmsfBase) + "/ue-contexts/" + supi + "/sendsms";
    post.headers.emplace("content-type", encoded.content_type_header);
    post.headers.emplace("authorization", "Bearer " + token);
    post.body = encoded.body;
    auto resp = client.send(post);
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 200) << resp->body;
    EXPECT_EQ(json::parse(resp->body).at("deliveryStatus"), "SMS_DELIVERY_SMSF_ACCEPTED");
}

// Nsmsf_SMService_Activate for imsi-<imsi>, with a GPSI in the UE SMS context.
void activate(sbi_core::http2::Client& client, const std::string& token, const std::string& imsi) {
    const std::string supi = std::string("imsi-") + imsi;
    sbi_core::http2::ClientRequest put;
    put.method = "PUT";
    put.url = std::string(kSmsfBase) + "/ue-contexts/" + supi;
    put.headers.emplace("content-type", "application/json");
    put.headers.emplace("authorization", "Bearer " + token);
    put.body = json{{"supi", supi},
                    {"amfId", "00000000-0000-4000-8000-000000000aaa"},
                    {"accessType", "3GPP_ACCESS"},
                    {"gpsi", "msisdn-491701234567"}}
                   .dump();
    bool put_ok = false;
    for (int attempt = 0; attempt < 100 && !put_ok; ++attempt) {
        auto r = client.send(put);
        put_ok = r && (r->status == 201 || r->status == 204);
        if (!put_ok) {
            std::this_thread::sleep_for(100ms);
        }
    }
    ASSERT_TRUE(put_ok) << "SMServiceActivation failed";
}

// Nsmsf_SMService_Activate, then Nsmsf_SMService_UplinkSMS carrying CP-DATA/RP-DATA/SMS-SUBMIT.
void activate_and_send(sbi_core::http2::Client& client,
                       const std::string& token,
                       const std::string& imsi) {
    ASSERT_NO_FATAL_FAILURE(activate(client, token, imsi));
    sms_core::RpMessage rp;
    rp.mti = sms_core::RpMti::DataMsToNetwork;
    rp.message_reference = 0x2A;
    rp.destination_address = {0x91, 0x44, 0x77, 0x58, 0x10, 0x06, 0x50};
    rp.user_data = kSubmit;
    sms_core::CpMessage cp;
    cp.ti_value = 2;
    cp.rpdu = sms_core::encode_rp(rp);
    ASSERT_NO_FATAL_FAILURE(
        send_uplink(client, token, std::string("imsi-") + imsi, sms_core::encode_cp(cp)));
}

template <typename Event> std::vector<Event> events_of(const std::vector<li_core::Pdu>& pdus) {
    std::vector<Event> out;
    for (const auto& pdu : pdus) {
        if (const auto d = xiri::decode_xiri_payload(pdu.payload); d.has_value()) {
            if (const auto* e = std::get_if<Event>(&d->event)) {
                out.push_back(*e);
            }
        }
    }
    return out;
}

void check_downlink(FakeAmf& amf, const std::string& imsi) {
    ASSERT_TRUE(amf.calls.wait_for(2, 15s)) << "AMF never received CP-ACK + RP reply";
    const auto calls = amf.calls.take();
    for (const auto& c : calls) {
        EXPECT_EQ(c.ue_context_id, std::string("imsi-") + imsi);
        EXPECT_EQ(c.body.at("n1MessageContainer").at("n1MessageClass"), "SMS");
    }
    EXPECT_EQ(calls[0].n1, (Bytes{0xA9, 0x04})); // CP-ACK, TI 2, flag set
    EXPECT_FALSE(calls[0].body.value("lastMsgIndication", false));
    EXPECT_TRUE(calls[1].body.value("lastMsgIndication", false));
    auto cp = sms_core::decode_cp(calls[1].n1);
    ASSERT_TRUE(cp.has_value());
    auto rp = sms_core::decode_rp(cp->rpdu);
    ASSERT_TRUE(rp.has_value());
    EXPECT_EQ(rp->mti, sms_core::RpMti::ErrorNetworkToMs);
    EXPECT_EQ(rp->message_reference, 0x2A);
    EXPECT_EQ(rp->cause, 38);
}

struct Lab {
    Inbox<li_core::Pdu> mdf2;
    li_core::X2X3Server mdf2_server{
        fake_mdf2_config(), [this](const li_core::Pdu& pdu, std::string_view) { mdf2.push(pdu); }};
    FakeAmf amf;
};

} // namespace

TEST(LiSmsfEndToEnd, RealSmsfRepliesViaAmfAndReportsSmsMessageAndSmsReportForATarget) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port(), lab.amf.port());
    ::setenv("SMSF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess smsf{SMSF_PATH};
    // ADR-0499 phase 5: the NRF issues a token towards AMFs only if one is registered -- the fake
    // AMF here, as a real one would be.
    nf_test::register_test_nf("00000000-0000-4000-8000-000000000aaa", "AMF");
    ::unsetenv("SMSF_CONFIG_FILE");
    ASSERT_GT(smsf.pid(), 0);

    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate("e0e1e2e3-e4e5-46e7-88e9-eaebecedeeef", false)));
    auto client = make_client();
    const auto token = fetch_token(client);
    ASSERT_FALSE(token.empty());
    activate_and_send(client, token, kTargetImsi);

    check_downlink(lab.amf, kTargetImsi);

    ASSERT_TRUE(lab.mdf2.wait_for(2, 15s)) << "expected SMSMessage + SMSReport";
    const auto pdus = lab.mdf2.take();
    const auto msgs = events_of<xiri::SmsMessage>(pdus);
    ASSERT_EQ(msgs.size(), 1U);
    const auto& m = msgs[0];
    ASSERT_TRUE(m.originating_party.supi.has_value());
    EXPECT_EQ(std::get<xiri::Imsi>(*m.originating_party.supi).digits, kTargetImsi);
    ASSERT_TRUE(m.originating_party.gpsi.has_value()); // from the UE SMS context
    EXPECT_EQ(std::get<xiri::Msisdn>(*m.originating_party.gpsi).digits, "491701234567");
    EXPECT_EQ(m.terminating_party.sms_address, kTpDaField);
    EXPECT_EQ(m.direction, xiri::SmsDirection::FromTarget);
    EXPECT_EQ(m.link_transfer_status, xiri::SmsTransferStatus::TransferFailed);
    EXPECT_EQ(m.message_type, xiri::SmsMessageType::Submit);
    EXPECT_EQ(m.rp_message_reference, 0x2A);
    ASSERT_TRUE(m.tpdu_data.has_value());
    EXPECT_FALSE(m.tpdu_data->redacted);
    EXPECT_EQ(m.tpdu_data->tpdu, kSubmit);
    ASSERT_TRUE(m.location.has_value());
    ASSERT_TRUE(m.location->user_location->nr.has_value());
    EXPECT_EQ(m.location->user_location->nr->ncgi.nr_cell_id, 0x10U);

    const auto reports = events_of<xiri::SmsReport>(pdus);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].message_type, xiri::SmsMessageType::SubmitReportError);
    EXPECT_EQ(reports[0].rp_message_reference, 0x2A);
    EXPECT_EQ(reports[0].tpdu_data.tpdu[1], 0xC2); // TP-FCS
    lab.mdf2_server.stop();
}

TEST(LiSmsfEndToEnd, TruncateTpUserDataDeliversARedactedTpdu) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port(), lab.amf.port());
    ::setenv("SMSF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess smsf{SMSF_PATH};
    // ADR-0499 phase 5: the NRF issues a token towards AMFs only if one is registered -- the fake
    // AMF here, as a real one would be.
    nf_test::register_test_nf("00000000-0000-4000-8000-000000000aaa", "AMF");
    ::unsetenv("SMSF_CONFIG_FILE");

    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate("e1e1e2e3-e4e5-46e7-88e9-eaebecedeeef", true)));
    auto client = make_client();
    const auto token = fetch_token(client);
    ASSERT_FALSE(token.empty());
    activate_and_send(client, token, kTargetImsi);

    ASSERT_TRUE(lab.mdf2.wait_for(1, 15s));
    const auto msgs = events_of<xiri::SmsMessage>(lab.mdf2.take());
    ASSERT_EQ(msgs.size(), 1U);
    ASSERT_TRUE(msgs[0].tpdu_data.has_value());
    EXPECT_TRUE(msgs[0].tpdu_data->redacted);
    const auto expected = sms_core::redact_tpdu(kSubmit, sms_core::Direction::MsToSc);
    ASSERT_TRUE(expected.has_value());
    EXPECT_EQ(msgs[0].tpdu_data->tpdu, *expected);
    EXPECT_NE(msgs[0].tpdu_data->tpdu, kSubmit); // the text itself never left the POI
    lab.mdf2_server.stop();
}

TEST(LiSmsfEndToEnd, NonTargetSmsIsRelayedButNotIntercepted) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port(), lab.amf.port());
    ::setenv("SMSF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess smsf{SMSF_PATH};
    // ADR-0499 phase 5: the NRF issues a token towards AMFs only if one is registered -- the fake
    // AMF here, as a real one would be.
    nf_test::register_test_nf("00000000-0000-4000-8000-000000000aaa", "AMF");
    ::unsetenv("SMSF_CONFIG_FILE");

    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate("e2e1e2e3-e4e5-46e7-88e9-eaebecedeeef", false)));
    auto client = make_client();
    const auto token = fetch_token(client);
    ASSERT_FALSE(token.empty());
    activate_and_send(client, token, kOtherImsi);

    check_downlink(lab.amf, kOtherImsi);
    EXPECT_FALSE(lab.mdf2.wait_for(1, 2s)) << "a non-target SMS produced an xIRI";
    lab.mdf2_server.stop();
}

// ADR-0486 increment 4, SMS-MT (TS 23.502 clause 4.13.3.6, TS 33.128 clause 6.2.5.3): the test is
// the SMS-GMSC (Nsmsf_SMService_MtForwardSm) and the AMF relaying the UE's CP-ACK and
// CP-DATA(RP-ACK [SMS-DELIVER-REPORT]). SMSMessage: deliver, toTarget, transferSucceeded,
// originating sMSAddress = TP-OA, otherMessage from TP-MMS. SMSReport: deliverReportAck with the
// UE's SMS-DELIVER-REPORT and the carrying UplinkSMS's ueLocation (NOTE 7).
TEST(LiSmsfEndToEnd, MtSmsToATargetReportsSmsMessageAndSmsReport) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port(), lab.amf.port());
    ::setenv("SMSF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess smsf{SMSF_PATH};
    // ADR-0499 phase 5: the NRF issues a token towards AMFs only if one is registered -- the fake
    // AMF here, as a real one would be.
    nf_test::register_test_nf("00000000-0000-4000-8000-000000000aaa", "AMF");
    ::unsetenv("SMSF_CONFIG_FILE");

    auto admf = make_client();
    ASSERT_TRUE(post_x1(admf, x1_activate("e3e1e2e3-e4e5-46e7-88e9-eaebecedeeef", false)));
    auto client = make_client();
    const auto token = fetch_token(client);
    ASSERT_FALSE(token.empty());
    ASSERT_NO_FATAL_FAILURE(activate(client, token, kTargetImsi));
    const std::string supi = std::string("imsi-") + kTargetImsi;

    // SMS-DELIVER from TP-OA "46708251358", TP-MMS=1 (no more messages), inside RP-DATA (MR 0x11)
    // inside the SMS-GMSC's CP-DATA (TI 3).
    const Bytes deliver = {0x04, 0x0B, 0x91, 0x64, 0x07, 0x28, 0x15, 0x53, 0xF8, 0x00,
                           0x00, 0x62, 0x01, 0x41, 0x21, 0x43, 0x65, 0x00, 0x0A, 0xE8,
                           0x32, 0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};
    sms_core::RpMessage rp;
    rp.mti = sms_core::RpMti::DataNetworkToMs;
    rp.message_reference = 0x11;
    rp.originator_address = {0x91, 0x44, 0x77, 0x58, 0x10, 0x06, 0x50};
    rp.user_data = deliver;
    sms_core::CpMessage gmsc_cp;
    gmsc_cp.ti_value = 3;
    gmsc_cp.rpdu = sms_core::encode_rp(rp);
    const auto gmsc_payload = sms_core::encode_cp(gmsc_cp);

    sbi_core::multipart::Part json_part;
    json_part.content_type = "application/json";
    json_part.body = R"({"smsPayload":{"contentId":"sms"}})";
    sbi_core::multipart::Part bin_part;
    bin_part.content_type = "application/vnd.3gpp.sms";
    bin_part.content_id = "sms";
    bin_part.body.assign(gmsc_payload.begin(), gmsc_payload.end());
    const auto encoded = sbi_core::multipart::encode({json_part, bin_part});
    sbi_core::http2::ClientRequest mt;
    mt.method = "POST";
    mt.url = std::string(kSmsfBase) + "/ue-contexts/" + supi + "/send-mt-sms";
    mt.headers.emplace("content-type", encoded.content_type_header);
    mt.headers.emplace("authorization", "Bearer " + token);
    mt.body = encoded.body;
    tl::expected<sbi_core::http2::ClientResponse, std::string> mt_resp =
        tl::unexpected(std::string("not sent"));
    std::thread gmsc([&] {
        auto gmsc_client = make_client();
        mt_resp = gmsc_client.send(mt);
    });

    ASSERT_TRUE(lab.amf.calls.wait_for(1, 15s)) << "the SMSF never sent the MT CP-DATA";
    const auto down = sms_core::decode_cp(lab.amf.calls.take()[0].n1);
    ASSERT_TRUE(down.has_value());
    ASSERT_EQ(down->type, sms_core::CpMessageType::Data);

    // The AMF relaying the UE: CP-ACK, then CP-DATA(RP-ACK [SMS-DELIVER-REPORT]).
    ASSERT_NO_FATAL_FAILURE(
        send_uplink(client, token, supi, sms_core::encode_cp(sms_core::make_cp_ack(*down))));
    sms_core::RpMessage rp_ack;
    rp_ack.mti = sms_core::RpMti::AckMsToNetwork;
    rp_ack.message_reference = 0x11;
    rp_ack.user_data = Bytes{0x00, 0x00};
    ASSERT_NO_FATAL_FAILURE(send_uplink(
        client,
        token,
        supi,
        sms_core::encode_cp(sms_core::make_cp_data_reply(*down, sms_core::encode_rp(rp_ack)))));

    gmsc.join();
    ASSERT_TRUE(mt_resp.has_value()) << mt_resp.error();
    ASSERT_EQ(mt_resp->status, 200) << mt_resp->body;
    ASSERT_TRUE(lab.amf.calls.wait_for(2, 15s)) << "no closing CP-ACK";
    const auto closing = lab.amf.calls.take()[1];
    EXPECT_EQ(sms_core::decode_cp(closing.n1)->type, sms_core::CpMessageType::Ack);
    EXPECT_TRUE(closing.body.value("lastMsgIndication", false));

    ASSERT_TRUE(lab.mdf2.wait_for(2, 15s)) << "expected SMSMessage + SMSReport";
    const auto pdus = lab.mdf2.take();
    const auto msgs = events_of<xiri::SmsMessage>(pdus);
    ASSERT_EQ(msgs.size(), 1U);
    const auto& m = msgs[0];
    EXPECT_EQ(m.originating_party.sms_address, kTpDaField); // TP-OA: same digits as kSubmit's DA
    ASSERT_TRUE(m.terminating_party.supi.has_value());
    EXPECT_EQ(std::get<xiri::Imsi>(*m.terminating_party.supi).digits, kTargetImsi);
    ASSERT_TRUE(m.terminating_party.gpsi.has_value());
    EXPECT_EQ(m.direction, xiri::SmsDirection::ToTarget);
    EXPECT_EQ(m.link_transfer_status, xiri::SmsTransferStatus::TransferSucceeded);
    EXPECT_EQ(m.other_message, false);
    EXPECT_EQ(m.message_type, xiri::SmsMessageType::Deliver);
    EXPECT_EQ(m.rp_message_reference, 0x11);
    ASSERT_TRUE(m.tpdu_data.has_value());
    EXPECT_EQ(m.tpdu_data->tpdu, deliver);

    const auto reports = events_of<xiri::SmsReport>(pdus);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].message_type, xiri::SmsMessageType::DeliverReportAck);
    EXPECT_EQ(reports[0].rp_message_reference, 0x11);
    EXPECT_EQ(reports[0].tpdu_data.tpdu, (Bytes{0x00, 0x00}));
    ASSERT_TRUE(reports[0].location.has_value());
    lab.mdf2_server.stop();
}
