// ADR-0488: the CHF IRI-POI (TS 33.128 clause 7.19) at the REAL chf process. The test is the
// charging trigger function (an SMF sending Nchf_ConvergedCharging / Nchf_OfflineOnlyCharging
// ChargingDataRequests), the warrant is provisioned over real LI_X1 with the ChargingDataEvents
// extension, and the xIRIs are read off a loopback MDF2's real LI_X2 server. The POI fires on
// RECEIPT of the request (clause 7.19.3.1), so the CHF's own charging backends do not need to be up
// for what is asserted here -- whatever the CHF answers, the record is about what it received.

#include "sbi_core/http2_client.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "diameter_core/avp.hpp"
#include "diameter_core/dictionary.hpp"
#include "diameter_core/header.hpp"
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

constexpr const char* kTargetImsi = "999700000000051";
constexpr std::uint16_t kX1Port = 19864;
constexpr const char* kX1Url = "https://127.0.0.1:19864/X1/NE";
constexpr const char* kNeId = "chf-poi-e2e";
constexpr const char* kChfBase = "https://127.0.0.1:7784";

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
    cfg.server_cert_path = CERTS_DIR "/chf/cert.pem";
    cfg.server_key_path = CERTS_DIR "/chf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

// config/chf.json with li_poi enabled toward the loopback MDF2. CDR sink: direct-insert off and an
// unreachable event-bus broker -- the disclosed workaround test_chf_diameter_rar.cpp and
// scripts/run-chf-rating-baseline.sh already use, since the CHF FATALs at startup without a
// reachable Doris and this test needs no CDR.
std::string write_config(std::uint16_t mdf2_port) {
    std::ifstream in(CHF_CONFIG_TEMPLATE);
    json cfg = json::parse(in);
    // The CI TSan leg sets CHF_CDR_EVENT_BUS_BROKERS empty (librdkafka cannot run under TSan) and
    // that environment value overrides this file: with direct-insert off too, the CHF would have
    // no CDR sink and exit at startup. There the CHF keeps its default direct-insert sink (CI's
    // Doris, CHF_DORIS_*); everywhere else, the unreachable-broker workaround above.
    const char* brokers_env = std::getenv("CHF_CDR_EVENT_BUS_BROKERS");
    const bool bus_forced_off = brokers_env != nullptr && *brokers_env == '\0';
    cfg["cdr_direct_insert"] = bus_forced_off;
    if (!bus_forced_off) {
        cfg["cdr_event_bus_brokers"] = "127.0.0.1:9";
    }
    auto& lp = cfg["li_poi"];
    lp["enabled"] = true;
    lp["x1_bind_address"] = "127.0.0.1";
    lp["x1_port"] = kX1Port;
    lp["ne_identifier"] = kNeId;
    lp["mdf2_host"] = "127.0.0.1";
    lp["mdf2_port"] = mdf2_port;
    lp["mdf2_sni"] = "localhost";
    const std::string path = std::string(::testing::TempDir()) + "chf_li_e2e.json";
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

std::string x1_activate(const std::string& xid, bool charging_data_events) {
    namespace x1 = li_core::x1;
    x1::TaskDetails task;
    task.xid = xid;
    task.targets.push_back({x1::TargetIdentifierKind::SupiImsi, "supiimsi", kTargetImsi});
    if (charging_data_events) {
        task.charging_data_events = x1::ChargingDataEventsGenerated::DefaultChargingDataInformation;
    }
    task.delivery = x1::DeliveryType::X2Only;
    task.dids = {"44444444-4444-4444-8444-444444444444"};
    x1::Request request;
    request.header = {"admf-test",
                      kNeId,
                      "2026-10-14T00:00:00.000000Z",
                      "v1.23.1",
                      "00000000-0000-4000-8000-0000000000f3"};
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

// A ChargingDataRequest (TS 32.291 6.1.6.2.1.1) for the target with PDU session charging
// information; `roamer` sets UserInformation.roamerInOut when non-empty.
json charging_data_request(const std::string& roamer) {
    json user{{"servedGPSI", "msisdn-491701230051"}};
    if (!roamer.empty()) {
        user["roamerInOut"] = roamer;
    }
    return json{
        {"subscriberIdentifier", std::string("imsi-") + kTargetImsi},
        {"nfConsumerIdentification", {{"nodeFunctionality", "SMF"}}},
        {"invocationTimeStamp", "2026-10-14T12:00:00Z"},
        {"invocationSequenceNumber", 0},
        {"pDUSessionChargingInformation",
         {{"chargingId", 7},
          {"userInformation", user},
          {"userLocationinfo",
           {{"nrLocation",
             {{"tai", {{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"tac", "000001"}}},
              {"ncgi",
               {{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"nrCellId", "000000010"}}}}}}},
          {"pduSessionInformation",
           {{"pduSessionID", 5},
            {"dnnId", "internet"},
            {"networkSlicingInfo", {{"sNSSAI", {{"sst", 1}}}}}}}}}};
}

void post_charging(const std::string& path, const json& body) {
    auto client = make_client();
    nf_test::attach_test_tokens(client); // ADR-0499: NFs enforce tokens
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string(kChfBase) + path;
    req.headers.emplace("content-type", "application/json");
    req.body = body.dump();
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (client.send(req).has_value()) {
            return; // the CHF's answer is not what this test is about
        }
        std::this_thread::sleep_for(100ms);
    }
    ADD_FAILURE() << "CHF never answered " << path;
}

std::vector<xiri::ChargingDataEvent> records(const std::vector<li_core::Pdu>& pdus) {
    std::vector<xiri::ChargingDataEvent> out;
    for (const auto& pdu : pdus) {
        if (const auto d = xiri::decode_xiri_payload(pdu.payload); d.has_value()) {
            if (const auto* e = std::get_if<xiri::ChargingDataEvent>(&d->event)) {
                out.push_back(*e);
            }
        }
    }
    return out;
}

// RFC 6733 CER, then an Rf ACR (TS 32.299 6.2.2): Session-Id, Origin-Host/-Realm, Accounting-
// Record-Type EVENT, Accounting-Record-Number, and Service-Information (873, 3GPP vendor) carrying
// Subscription-Id { END_USER_IMSI, the target IMSI } and an (empty) PS-Information (874).
std::vector<std::uint8_t> diameter_message(std::uint32_t command,
                                           std::uint32_t application,
                                           const std::vector<std::uint8_t>& avps) {
    diameter_core::Header h;
    h.flags = diameter_core::CommandFlag::kRequest;
    h.command_code = command;
    h.application_id = application;
    h.hop_by_hop_id = 0x0000C0DE;
    h.end_to_end_id = 0x0000FACE;
    auto msg = diameter_core::encode_header(h, static_cast<std::uint32_t>(avps.size()));
    msg.insert(msg.end(), avps.begin(), avps.end());
    return msg;
}

void put(std::vector<std::uint8_t>& out,
         std::uint32_t code,
         std::vector<std::uint8_t> data,
         std::uint32_t vendor = 0) {
    diameter_core::Avp avp;
    avp.code = code;
    avp.flags = diameter_core::AvpFlag::kMandatory;
    if (vendor != 0) {
        avp.flags |= diameter_core::AvpFlag::kVendor;
        avp.vendor_id = vendor;
    }
    avp.data = std::move(data);
    diameter_core::encode_avp(out, avp);
}

std::vector<std::uint8_t> build_acr() {
    namespace d = diameter_core;
    namespace dict = d::dictionary;
    std::vector<std::uint8_t> sub;
    put(sub,
        dict::Dcc::kSubscriptionIdType,
        d::encode_integer32(dict::Dcc::SubscriptionIdType::kEndUserImsi));
    put(sub, dict::Dcc::kSubscriptionIdData, d::encode_octet_string(kTargetImsi));
    std::vector<std::uint8_t> service;
    put(service, dict::Dcc::kSubscriptionId, sub);
    put(service, 874, {}, 10415); // PS-Information (TS 32.299 7.2.158), no members needed here
    std::vector<std::uint8_t> avps;
    put(avps, dict::Avp::kSessionId, d::encode_octet_string("li-chf-e2e;rf;1"));
    put(avps, dict::Avp::kOriginHost, d::encode_octet_string("smf-rf.example.com"));
    put(avps, dict::Avp::kOriginRealm, d::encode_octet_string("example.com"));
    put(avps,
        dict::Avp::kAccountingRecordType,
        d::encode_integer32(dict::BaseAccounting::AccountingRecordType::kEvent));
    put(avps, dict::Avp::kAccountingRecordNumber, d::encode_unsigned32(0));
    put(avps, 873, service, 10415); // Service-Information (TS 32.299 7.2.192)
    return diameter_message(dict::Command::kAccounting, dict::BaseAccounting::kApplicationId, avps);
}

// Sends `msg` on `sock` and reads (and discards) the one answer.
bool diameter_exchange(boost::asio::ip::tcp::socket& sock, const std::vector<std::uint8_t>& msg) {
    boost::system::error_code ec;
    boost::asio::write(sock, boost::asio::buffer(msg), ec);
    if (ec) {
        return false;
    }
    std::vector<std::uint8_t> header(20);
    boost::asio::read(sock, boost::asio::buffer(header), ec);
    if (ec) {
        return false;
    }
    std::size_t offset = 0;
    std::uint32_t avps_length = 0;
    if (!diameter_core::decode_header(header, offset, avps_length)) {
        return false;
    }
    std::vector<std::uint8_t> rest(avps_length);
    boost::asio::read(sock, boost::asio::buffer(rest), ec);
    return !ec;
}

struct Lab {
    Inbox mdf2;
    li_core::X2X3Server mdf2_server{
        fake_mdf2_config(), [this](const li_core::Pdu& pdu, std::string_view) { mdf2.push(pdu); }};
};

} // namespace

TEST(LiChfEndToEnd, ChargingDataRequestsForATargetAreReportedOnlyWithRoamerInOut) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port());
    ::setenv("CHF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess chf{CHF_PATH};
    ::unsetenv("CHF_CONFIG_FILE");
    ASSERT_GT(chf.pid(), 0);

    auto admf = make_client();

    nf_test::attach_test_tokens(admf); // ADR-0499: NFs enforce tokens
    ASSERT_TRUE(post_x1(admf, x1_activate("a0e1e2e3-e4e5-46e7-88e9-eaebecedeeef", true)));

    // 1. Converged, OUT_BOUND: reported. 2. No roamerInOut: withheld (ADR-0488, strict ASN.1).
    // 3. Offline-only, IN_BOUND: reported with the offline encapsulation.
    const json roaming = charging_data_request("OUT_BOUND");
    post_charging("/nchf-convergedcharging/v3/chargingdata", roaming);
    post_charging("/nchf-convergedcharging/v3/chargingdata", charging_data_request(""));
    const json inbound = charging_data_request("IN_BOUND");
    post_charging("/nchf-offlineonlycharging/v1/offlinechargingdata", inbound);

    ASSERT_TRUE(lab.mdf2.wait_for(2, 15s)) << "expected two ChargingDataEvent xIRIs";
    EXPECT_FALSE(lab.mdf2.wait_for(3, 2s)) << "the request without roamerInOut was reported";
    const auto recs = records(lab.mdf2.take());
    ASSERT_EQ(recs.size(), 2U);

    const auto& conv = std::get<xiri::NchfChargingDataRequest>(recs[0].information);
    EXPECT_FALSE(conv.offline);
    EXPECT_EQ(conv.charging_data.reference,
              "TS32291_Nchf_ConvergedCharging.yaml#/components/schemas/ChargingDataRequest");
    EXPECT_EQ(json::parse(conv.charging_data.value), roaming); // unmodified payload
    EXPECT_EQ(conv.charging_event, xiri::NchfChargingEvent::PduSessionChargingInformation);
    EXPECT_EQ(conv.roamer_in_out, xiri::RoamerInOut::OutBound);
    ASSERT_TRUE(recs[0].location.has_value());
    ASSERT_TRUE(recs[0].location->user_location->nr.has_value());
    EXPECT_EQ(recs[0].location->user_location->nr->ncgi.nr_cell_id, 0x10U);

    const auto& off = std::get<xiri::NchfChargingDataRequest>(recs[1].information);
    EXPECT_TRUE(off.offline);
    EXPECT_EQ(off.charging_data.reference,
              "TS32291_Nchf_OfflineOnlyCharging.yaml#/components/schemas/ChargingDataRequest");
    EXPECT_EQ(off.roamer_in_out, xiri::RoamerInOut::InBound);
    lab.mdf2_server.stop();
}

// TS 33.128 table 7.19.1-1: without the ChargingDataEvents extension "ChargingDataEvent records
// shall not be generated", even for a target.
TEST(LiChfEndToEnd, TaskWithoutChargingDataEventsExtensionGetsNoRecords) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port());
    ::setenv("CHF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess chf{CHF_PATH};
    ::unsetenv("CHF_CONFIG_FILE");

    auto admf = make_client();

    nf_test::attach_test_tokens(admf); // ADR-0499: NFs enforce tokens
    ASSERT_TRUE(post_x1(admf, x1_activate("a1e1e2e3-e4e5-46e7-88e9-eaebecedeeef", false)));
    post_charging("/nchf-convergedcharging/v3/chargingdata", charging_data_request("OUT_BOUND"));
    EXPECT_FALSE(lab.mdf2.wait_for(1, 3s)) << "a task without ChargingDataEvents got a record";
    lab.mdf2_server.stop();
}

// TS 33.128 clause 7.19.3.9: an Rf ACR for the target (Subscription-Id in Service-Information) ->
// ChargingDataEvent(rfChargingDataRequest) carrying the ACR's octets exactly as sent, with
// rfChargingEvent pS-Information. No roamerInOut on Rf, so nothing is withheld.
TEST(LiChfEndToEnd, RfAccountingRequestForATargetIsReportedWithItsOctets) {
    Lab lab;
    ASSERT_TRUE(lab.mdf2_server.start().has_value());
    const auto cfg = write_config(lab.mdf2_server.bound_port());
    ::setenv("CHF_CONFIG_FILE", cfg.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess chf{CHF_PATH};
    ::unsetenv("CHF_CONFIG_FILE");

    auto admf = make_client();

    nf_test::attach_test_tokens(admf); // ADR-0499: NFs enforce tokens
    ASSERT_TRUE(post_x1(admf, x1_activate("a2e1e2e3-e4e5-46e7-88e9-eaebecedeeef", true)));
    ASSERT_TRUE(nf_test::wait_tcp_listening(diameter_core::kDiameterTcpPort));

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock(ioc);
    boost::system::error_code ec;
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), diameter_core::kDiameterTcpPort}, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::vector<std::uint8_t> cer_avps;
    put(cer_avps,
        diameter_core::dictionary::Avp::kOriginHost,
        diameter_core::encode_octet_string("smf-rf.example.com"));
    put(cer_avps,
        diameter_core::dictionary::Avp::kOriginRealm,
        diameter_core::encode_octet_string("example.com"));
    ASSERT_TRUE(diameter_exchange(
        sock,
        diameter_message(diameter_core::dictionary::Command::kCapabilitiesExchange, 0, cer_avps)));
    const auto acr = build_acr();
    ASSERT_TRUE(diameter_exchange(sock, acr)) << "no ACA";

    ASSERT_TRUE(lab.mdf2.wait_for(1, 15s)) << "expected a ChargingDataEvent for the ACR";
    const auto recs = records(lab.mdf2.take());
    ASSERT_EQ(recs.size(), 1U);
    const auto* rf = std::get_if<xiri::RfChargingDataRequest>(&recs[0].information);
    ASSERT_NE(rf, nullptr);
    EXPECT_EQ(rf->acr, acr);
    EXPECT_EQ(rf->charging_event, xiri::RfChargingEvent::PsInformation);
    EXPECT_FALSE(recs[0].location.has_value());
    lab.mdf2_server.stop();
}
