// The AMF IRI-POI end to end through the REAL `amf` process: the harness ADR-0393 and ADR-0440 both
// named as their own increment ("That needs an AMF-process harness with the POI on, which no
// existing test has"). test_li_amf_poi.cpp drives li_poi.cpp's methods directly; this test instead
// spawns the full NF fleet with the AMF's li_poi enabled (a temp copy of config/amf.json, selected
// through nf_config's own AMF_CONFIG_FILE override), provisions a warrant over real LI_X1, drives a
// real UE over real NGAP/SCTP + NAS (register -> PDU session -> deregister), and asserts on the
// xIRIs a loopback MDF2 actually receives over real LI_X2.
//
// What this proves that the unit-scope test cannot: the hooks inside ngap_task.cpp's
// RegistrationAccept site and handle_uplink_nas_transport_deregistration really fire, with real
// AMF state -- the SUPI from authentication, the 5G-GUTI the AMF really assigned (cross-checked
// against the 5G-TMSI the UE decoded from its own RegistrationAccept), and the location parsed from
// the UplinkNASTransport's real NGAP UserLocationInformation (the gNB driver's fixed NR cell).
//
// Gating is "All" (TS 33.128 table 6.2.2.1.1-2), so every wired AMF record is generated:
// Registration
// + IdentifierAssociation at RegistrationAccept, Deregistration + IdentifierDeassociation at
// deregistration. A second test drives a real N2 handover (two gNBs, a real UPF) and asserts the
// AMFLocationUpdate that HandoverNotify triggers (ADR-0460). Not covered here, disclosed:
// LocationUpdate on N2 PathSwitchRequest, and StartOfInterceptionWithRegisteredUE (not wired).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "li_core/x2x3_pdu.hpp"
#include "li_core/x2x3_server.hpp"
#include "li_core/xiri.hpp"
#include "ngap_test_gnb.hpp"
#include "spawn_guard.hpp"
#include "ue_nas_driver.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nf_test::NgapTestGnb;
namespace xiri = li_core::xiri;

constexpr const char* kAmfNgapAddress = "127.0.0.5";
constexpr std::uint16_t kAmfNgapPort = 38412;
constexpr std::uint32_t kGnbId = 0x000044; // clear of the other NGAP tests' gNB IDs
constexpr std::uint32_t kSourceGnbId = 0x000045;
constexpr std::uint32_t kTargetGnbId = 0x000046;
constexpr std::uint32_t kTargetRanUeId = 7777;
constexpr std::uint64_t kTargetCellId = 0x2A; // not the driver's default cell 1: provably the target's
constexpr const char* kDnn = "internet";
constexpr std::uint8_t kSst = 1;
constexpr std::uint32_t kSd = 0x000001;
constexpr const char* kServingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";

// The AMF's own LI_X1 listener for this test -- a fixed port (the sbi server binds a fixed one),
// distinct from test_li_amf_poi.cpp's 19807 and config/amf.json's 7807.
constexpr std::uint16_t kX1Port = 19821;
constexpr const char* kX1Url = "https://127.0.0.1:19821/X1/NE";
constexpr const char* kNeId = "amf-poi-e2e";
constexpr const char* kXid = "c0c1c2c3-c4c5-46c7-88c9-cacbcccdcecf";
constexpr const char* kTargetImsiDigits = "999700000000001"; // nf_test::kTestSupi's digits

constexpr const char* kAusfProbe =
    "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session";
constexpr const char* kUdmProbe = "https://127.0.0.1:7780/nudm-ueau/v1/nonexistent/nonexistent";
constexpr const char* kUdrProbe =
    "https://127.0.0.1:7781/nudr-dr/v1/subscription-data/nonexistent/nonexistent";
constexpr const char* kPcfProbe = "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/none";
constexpr const char* kSmfProbe =
    "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve";

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
    cfg.server_cert_path = CERTS_DIR "/amf/cert.pem";
    cfg.server_key_path = CERTS_DIR "/amf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

// config/amf.json with only li_poi changed: enabled, this test's X1 port/NE id, and the loopback
// MDF2's ephemeral port. Everything else is the real default the rest of the suite runs with.
std::string write_li_enabled_amf_config(std::uint16_t mdf2_port) {
    std::ifstream in(AMF_CONFIG_TEMPLATE);
    nlohmann::json cfg = nlohmann::json::parse(in);
    auto& lp = cfg["li_poi"];
    lp["enabled"] = true;
    lp["x1_bind_address"] = "127.0.0.1";
    lp["x1_port"] = kX1Port;
    lp["ne_identifier"] = kNeId;
    lp["mdf2_host"] = "127.0.0.1";
    lp["mdf2_port"] = mdf2_port;
    lp["mdf2_sni"] = "localhost"; // the lab amf cert's SAN
    const std::string path = std::string(::testing::TempDir()) + "amf_li_e2e.json";
    std::ofstream(path) << cfg.dump(2);
    return path;
}

sbi_core::http2::Client make_client(const char* nf) {
    sbi_core::http2::TlsConfig tls{
        .cert_path = std::string(CERTS_DIR "/") + nf + "/cert.pem",
        .key_path = std::string(CERTS_DIR "/") + nf + "/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

std::string x1_activate_all() {
    return std::string(
               R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:c="http://uri.etsi.org/03280/common/2017/07">
  <x1RequestMessage xsi:type="ActivateTaskRequest">
    <admfIdentifier>admf-test</admfIdentifier>
    <neIdentifier>)") +
           kNeId + R"(</neIdentifier>
    <messageTimestamp>2026-10-05T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>00000000-0000-4000-8000-0000000000e1</x1TransactionId>
    <taskDetails>
      <xId>)" +
           kXid +
           R"(</xId>
      <targetIdentifiers>
        <targetIdentifier><supiimsi>)" +
           kTargetImsiDigits + R"(</supiimsi></targetIdentifier>
      </targetIdentifiers>
      <deliveryType>X2Only</deliveryType>
      <listOfDIDs><dId>22222222-2222-4222-8222-222222222222</dId></listOfDIDs>
      <taskDetailsExtensions>
        <Owner>3GPP</Owner>
        <tgpp:IdentifierAssociationExtensions xmlns:tgpp="urn:3GPP:ns:li:3GPPX1Extensions:r19:v4">
          <tgpp:IdentifierAssociationEventsGenerated>All</tgpp:IdentifierAssociationEventsGenerated>
        </tgpp:IdentifierAssociationExtensions>
      </taskDetailsExtensions>
    </taskDetails>
  </x1RequestMessage>
</X1Request>)";
}

void wait_for_sbi_peers(const std::vector<const char*>& urls) {
    auto client = make_client("hello-nf");
    for (const char* url : urls) {
        bool reachable = false;
        for (int attempt = 0; attempt < 200 && !reachable; ++attempt) {
            sbi_core::http2::ClientRequest req;
            req.method = "GET";
            req.url = url;
            reachable = client.send(req).has_value();
            if (!reachable) {
                std::this_thread::sleep_for(50ms);
            }
        }
        ASSERT_TRUE(reachable) << "NF never became reachable: " << url;
    }
}

struct RegisteredUe {
    std::uint64_t amf_ue_id = 0;
    nf_test::NasKeys keys;
    std::vector<std::uint8_t> guti_value;
};

// Same flow as test_amf_deregistration.cpp's register_ue (duplicated per this suite's per-TU
// convention).
void register_ue(NgapTestGnb& gnb, std::uint32_t ran_ue_id, RegisteredUe& out) {
    gnb.send_raw(gnb.build_initial_ue_message(
        ran_ue_id, nf_test::build_registration_request(nf_test::kTestSupi)));

    const auto challenge_pdu = gnb.receive_raw();
    ASSERT_FALSE(challenge_pdu.empty());
    NgapTestGnb::DownlinkNas challenge_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(challenge_pdu, challenge_nas));
    const auto challenge = nf_test::parse_authentication_request(challenge_nas.nas_pdu);
    ASSERT_TRUE(challenge.has_value());
    const auto res_star = nf_test::compute_res_star(*challenge, kServingNetworkName);
    ASSERT_TRUE(res_star.has_value());
    gnb.send_raw(gnb.build_uplink_nas_transport(
        challenge_nas.amf_ue_id, ran_ue_id, nf_test::build_authentication_response(*res_star)));

    const auto smc_pdu = gnb.receive_raw();
    ASSERT_FALSE(smc_pdu.empty());
    NgapTestGnb::DownlinkNas smc_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(smc_pdu, smc_nas));

    out.amf_ue_id = challenge_nas.amf_ue_id;
    out.keys = nf_test::derive_nas_keys(*challenge, nf_test::kTestSupi, kServingNetworkName);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        challenge_nas.amf_ue_id,
        ran_ue_id,
        nf_test::build_security_mode_complete(out.keys, /*uplink_count=*/0)));

    const auto accept_pdu = gnb.receive_raw();
    ASSERT_FALSE(accept_pdu.empty());
    NgapTestGnb::DownlinkNas accept_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(accept_pdu, accept_nas));
    const auto accept_plain =
        nf_test::open_secured_downlink(out.keys, /*downlink_count=*/1, accept_nas.nas_pdu);
    ASSERT_TRUE(accept_plain.has_value());
    ASSERT_GE(accept_plain->size(), 3u);
    ASSERT_EQ((*accept_plain)[2], 0x42) << "expected a RegistrationAccept";
    const auto guti_value = nf_test::extract_guti_from_registration_accept(*accept_plain);
    ASSERT_TRUE(guti_value.has_value());
    out.guti_value = *guti_value;
}

void establish_pdu_session(NgapTestGnb& gnb, const RegisteredUe& ue, std::uint32_t ran_ue_id) {
    gnb.send_raw(gnb.build_uplink_nas_transport(
        ue.amf_ue_id, ran_ue_id, nf_test::build_registration_complete(ue.keys, 1)));
    gnb.send_raw(gnb.build_uplink_nas_transport(
        ue.amf_ue_id,
        ran_ue_id,
        nf_test::build_pdu_session_establishment_request(
            ue.keys, /*uplink_count=*/2, /*pdu_session_id=*/5, /*pti=*/1, kDnn, kSst, kSd)));
    const auto accept_pdu = gnb.receive_raw();
    ASSERT_FALSE(accept_pdu.empty()) << "no PDU Session Establishment Accept";
}

// TS 24.501 9.11.3.4: a 5G-GUTI mobile identity's value ends with the 4-octet 5G-TMSI.
std::uint32_t tmsi_of(const std::vector<std::uint8_t>& guti_value) {
    const auto n = guti_value.size();
    return (std::uint32_t{guti_value[n - 4]} << 24) | (std::uint32_t{guti_value[n - 3]} << 16) |
           (std::uint32_t{guti_value[n - 2]} << 8) | std::uint32_t{guti_value[n - 1]};
}

// The gNB driver's fixed NR cell (ngap_test_gnb.cpp fill_user_location): PLMN 999/70, TAC
// 00 00 01, NR cell identity 1.
void expect_driver_cell(const xiri::Location& loc) {
    ASSERT_TRUE(loc.user_location.has_value());
    ASSERT_TRUE(loc.user_location->nr.has_value());
    const auto& nr = *loc.user_location->nr;
    EXPECT_EQ(nr.tai.plmn.mcc, "999");
    EXPECT_EQ(nr.tai.plmn.mnc, "70");
    EXPECT_EQ(nr.tai.tac, (std::vector<std::uint8_t>{0x00, 0x00, 0x01}));
    EXPECT_EQ(nr.ncgi.nr_cell_id, 1u);
}

std::string imsi_digits(const xiri::Supi& supi) {
    return std::holds_alternative<xiri::Imsi>(supi) ? std::get<xiri::Imsi>(supi).digits : "";
}

using nlohmann::json;

std::string fetch_smf_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=nsmf-pdusession&"
               "targetNfType=SMF";
    auto resp = client.send(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

// Same minimal CreateSmContext body test_amf_ngap_handover.cpp builds (duplicated per this
// suite's per-TU convention).
sbi_core::multipart::Encoded encode_create_sm_context_body(const std::string& supi,
                                                           std::int64_t pdu_session_id) {
    sbi_core::multipart::Part part;
    part.content_type = "application/json";
    part.body = json{
        {"servingNfId", "00000000-0000-4000-8000-0000000000aa"},
        {"servingNetwork", json{{"mcc", "999"}, {"mnc", "70"}}},
        {"anType", "3GPP_ACCESS"},
        {"smContextStatusUri", "https://example.com/sm-status"},
        {"supi", supi},
        {"pduSessionId", pdu_session_id},
        {"dnn", kDnn},
        {"sNssai", json{{"sst", kSst}}},
    }.dump();
    return sbi_core::multipart::encode({part});
}

// Blocks until SMF holds a real UPF N3 tunnel, i.e. can answer HANDOVER_REQUIRED with a real
// transfer (ADR-0267's trap: a PDU session created before the PFCP association carries no tunnel
// forever). Must run BEFORE the UE's own PDU session. See test_amf_ngap_handover.cpp for the
// full rationale; this is the same gate on the real thing, not a guessed sleep.
void wait_for_upf_sx_association(int max_attempts = 40) {
    auto client = make_client("hello-nf");
    // NRF is not among wait_for_sbi_peers' probes, so its token endpoint may not be up yet.
    std::string token;
    for (int attempt = 0; attempt < 100 && token.empty(); ++attempt) {
        token = fetch_smf_token(client);
        if (token.empty()) {
            std::this_thread::sleep_for(100ms);
        }
    }
    ASSERT_FALSE(token.empty()) << "failed to obtain an OAuth2 token from NRF for SMF";
    const std::string supi = "imsi-999700000000042"; // not the UE's own SUPI
    std::string last_failure = "never attempted";
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const auto encoded = encode_create_sm_context_body(supi, 100 + attempt);
        sbi_core::http2::ClientRequest create_req;
        create_req.method = "POST";
        create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
        create_req.headers.emplace("content-type", encoded.content_type_header);
        create_req.headers.emplace("authorization", "Bearer " + token);
        create_req.body = encoded.body;
        auto create_resp = client.send(create_req);
        ASSERT_TRUE(create_resp.has_value()) << "SMF stopped answering CreateSmContext entirely";
        ASSERT_EQ(create_resp->status, 201) << create_resp->body;
        const auto location = create_resp->headers.find("location");
        ASSERT_NE(location, create_resp->headers.end());
        const std::string ref = location->second.substr(location->second.rfind('/') + 1);

        sbi_core::http2::ClientRequest ho_req;
        ho_req.method = "POST";
        ho_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + ref + "/modify";
        ho_req.headers.emplace("content-type", "application/json");
        ho_req.headers.emplace("authorization", "Bearer " + token);
        ho_req.body = json{{"n2SmInfoType", "HANDOVER_REQUIRED"}}.dump();
        auto resp = client.send(ho_req);
        ASSERT_TRUE(resp.has_value());
        if (resp->status == 200) {
            return;
        }
        last_failure = std::to_string(resp->status) + " " + resp->body;
        std::this_thread::sleep_for(250ms);
    }
    FAIL() << "SMF never answered HANDOVER_REQUIRED with a real transfer. Last answer: "
           << last_failure;
}


} // namespace

TEST(LiAmfEndToEnd, RealAmfEmitsRegistrationAndDeregistrationXirisForATarget) {
    Collector mdf2;
    li_core::X2X3Server mdf2_server(fake_mdf2_config(), std::ref(mdf2));
    ASSERT_TRUE(mdf2_server.start().has_value());
    ASSERT_NE(mdf2_server.bound_port(), 0);

    const auto amf_config = write_li_enabled_amf_config(mdf2_server.bound_port());
    ::setenv("AMF_CONFIG_FILE", amf_config.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess udr{UDR_PATH};
    nf_test::SpawnedProcess udm{UDM_PATH};
    nf_test::SpawnedProcess ausf{AUSF_PATH};
    nf_test::SpawnedProcess pcf{PCF_PATH};
    nf_test::SpawnedProcess smf{SMF_PATH};
    nf_test::SpawnedProcess amf{AMF_PATH};
    ::unsetenv("AMF_CONFIG_FILE");
    ASSERT_GT(amf.pid(), 0);
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmfProbe}));

    // 1. The ADMF provisions an "All"-gated warrant on the AMF's own LI_X1 listener.
    auto admf = make_client("amf");
    sbi_core::http2::ClientRequest x1;
    x1.method = "POST";
    x1.url = kX1Url;
    x1.headers.emplace("content-type", "application/xml");
    x1.body = x1_activate_all();
    sbi_core::http2::ClientResponse activate;
    bool answered = false;
    for (int attempt = 0; attempt < 100 && !answered; ++attempt) {
        if (auto r = admf.send(x1); r.has_value()) {
            activate = *r;
            answered = true;
        } else {
            std::this_thread::sleep_for(100ms);
        }
    }
    ASSERT_TRUE(answered) << "the AMF's LI_X1 listener never answered -- li_poi not enabled?";
    ASSERT_EQ(activate.status, 200) << activate.body;
    ASSERT_NE(activate.body.find("ActivateTaskResponse"), std::string::npos) << activate.body;

    // 2. A real UE registers through the real AMF.
    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kGnbId));
    constexpr std::uint32_t kRanUeId = 1;
    RegisteredUe ue;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, kRanUeId, ue));
    ASSERT_GE(ue.guti_value.size(), 4u);
    const std::uint32_t assigned_tmsi = tmsi_of(ue.guti_value);

    ASSERT_TRUE(mdf2.wait_for(2, 10s))
        << "expected AMFRegistration + AMFIdentifierAssociation at RegistrationAccept";

    // 3. PDU session (Deregistration is only reachable from Phase::Done, ADR-0393), then a real
    // normal deregistration.
    ASSERT_NO_FATAL_FAILURE(establish_pdu_session(gnb, ue, kRanUeId));
    gnb.send_raw(gnb.build_uplink_nas_transport(
        ue.amf_ue_id,
        kRanUeId,
        nf_test::build_deregistration_request(
            ue.keys, /*uplink_count=*/3, ue.guti_value, /*switch_off=*/false)));
    ASSERT_FALSE(gnb.receive_raw().empty()) << "no DeregistrationAccept";
    ASSERT_FALSE(gnb.receive_raw().empty()) << "no UEContextReleaseCommand";
    gnb.send_raw(gnb.build_ue_context_release_complete(ue.amf_ue_id, kRanUeId));

    ASSERT_TRUE(mdf2.wait_for(4, 10s))
        << "expected AMFDeregistration + AMFIdentifierDeassociation at deregistration";
    std::this_thread::sleep_for(500ms); // let any stray extra PDU land before counting

    // 4. Exactly the four records, each with real AMF state.
    const auto pdus = mdf2.take();
    ASSERT_EQ(pdus.size(), 4u);
    int reg = 0, assoc = 0, dereg = 0, deassoc = 0;
    for (const auto& pdu : pdus) {
        EXPECT_EQ(pdu.type, li_core::PduType::X2);
        const auto matched =
            li_core::text_attribute(pdu, li_core::AttributeType::MatchedTargetIdentifier);
        ASSERT_TRUE(matched.has_value());
        EXPECT_NE(matched->find(kTargetImsiDigits), std::string::npos) << *matched;

        const auto decoded = xiri::decode_xiri_payload(pdu.payload);
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        if (const auto* r = std::get_if<xiri::AmfRegistration>(&decoded->event)) {
            ++reg;
            EXPECT_EQ(imsi_digits(r->supi), kTargetImsiDigits);
            EXPECT_EQ(r->guti.mcc, "999");
            EXPECT_EQ(r->guti.mnc, "70");
            EXPECT_EQ(r->guti.five_g_tmsi, assigned_tmsi)
                << "xIRI gUTI differs from the 5G-GUTI the UE was actually assigned";
        } else if (const auto* a = std::get_if<xiri::AmfIdentifierAssociation>(&decoded->event)) {
            ++assoc;
            EXPECT_EQ(imsi_digits(a->supi), kTargetImsiDigits);
            EXPECT_EQ(a->guti.five_g_tmsi, assigned_tmsi);
            expect_driver_cell(a->location);
        } else if (const auto* d = std::get_if<xiri::AmfDeregistration>(&decoded->event)) {
            ++dereg;
            EXPECT_EQ(d->deregistration_direction, xiri::AmfDirection::UeInitiated);
            ASSERT_TRUE(d->supi.has_value());
            EXPECT_EQ(imsi_digits(*d->supi), kTargetImsiDigits);
            ASSERT_TRUE(d->guti.has_value());
            EXPECT_EQ(d->guti->five_g_tmsi, assigned_tmsi);
            ASSERT_TRUE(d->switch_off_indicator.has_value());
            EXPECT_EQ(*d->switch_off_indicator, xiri::AmfDeregistration::SwitchOff::NormalDetach);
        } else if (const auto* x = std::get_if<xiri::AmfIdentifierDeassociation>(&decoded->event)) {
            ++deassoc;
            EXPECT_EQ(imsi_digits(x->supi), kTargetImsiDigits);
            EXPECT_EQ(x->guti.five_g_tmsi, assigned_tmsi);
        } else {
            ADD_FAILURE() << "unexpected xIRI record, variant index " << decoded->event.index();
        }
    }
    EXPECT_EQ(reg, 1);
    EXPECT_EQ(assoc, 1);
    EXPECT_EQ(dereg, 1);
    EXPECT_EQ(deassoc, 1);

    mdf2_server.stop();
}

// ADR-0460: AMFLocationUpdate through the real AMF on a real N2 handover (TS 33.128 6.2.2.2.4:
// "the N2 Handover Notify"). Two gNBs on two real SCTP associations, a real UPF (without one SMF
// has no N3 tunnel and the relay ends in HandoverPreparationFailure), the same "All" warrant.
// The target reports a distinctive NR cell (kTargetCellId, not the driver's default 1), so the
// assertion is that the xIRI's location is the one the AMF parsed out of THAT HandoverNotify --
// not the Registration's cell carried over.
TEST(LiAmfEndToEnd, RealAmfEmitsLocationUpdateXiriOnN2HandoverNotify) {
    Collector mdf2;
    li_core::X2X3Server mdf2_server(fake_mdf2_config(), std::ref(mdf2));
    ASSERT_TRUE(mdf2_server.start().has_value());

    const auto amf_config = write_li_enabled_amf_config(mdf2_server.bound_port());
    ::setenv("AMF_CONFIG_FILE", amf_config.c_str(), 1);
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess udr{UDR_PATH};
    nf_test::SpawnedProcess udm{UDM_PATH};
    nf_test::SpawnedProcess ausf{AUSF_PATH};
    nf_test::SpawnedProcess pcf{PCF_PATH};
    nf_test::SpawnedProcess upf{UPF_PATH};
    nf_test::SpawnedProcess smf{SMF_PATH};
    nf_test::SpawnedProcess amf{AMF_PATH};
    ::unsetenv("AMF_CONFIG_FILE");
    ASSERT_GT(amf.pid(), 0);
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmfProbe}));
    ASSERT_NO_FATAL_FAILURE(wait_for_upf_sx_association());

    auto admf = make_client("amf");
    sbi_core::http2::ClientRequest x1;
    x1.method = "POST";
    x1.url = kX1Url;
    x1.headers.emplace("content-type", "application/xml");
    x1.body = x1_activate_all();
    sbi_core::http2::ClientResponse activate;
    bool answered = false;
    for (int attempt = 0; attempt < 100 && !answered; ++attempt) {
        if (auto r = admf.send(x1); r.has_value()) {
            activate = *r;
            answered = true;
        } else {
            std::this_thread::sleep_for(100ms);
        }
    }
    ASSERT_TRUE(answered) << "the AMF's LI_X1 listener never answered -- li_poi not enabled?";
    ASSERT_EQ(activate.status, 200) << activate.body;

    NgapTestGnb source;
    ASSERT_TRUE(source.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(source.ng_setup(kSourceGnbId));
    NgapTestGnb target;
    ASSERT_TRUE(target.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(target.ng_setup(kTargetGnbId));

    constexpr std::uint32_t kUeRanId = 1;
    constexpr std::uint8_t kPduSessionId = 5;
    RegisteredUe ue;
    ASSERT_NO_FATAL_FAILURE(register_ue(source, kUeRanId, ue));
    ASSERT_NO_FATAL_FAILURE(establish_pdu_session(source, ue, kUeRanId));
    ASSERT_TRUE(mdf2.wait_for(2, 10s)) << "expected Registration + IdentifierAssociation first";

    // The target admits the handover on its own thread (AMF blocks for the answer inside the one
    // HandoverRequired call).
    std::string target_failure;
    std::thread target_thread([&] {
        const auto request = target.receive_raw();
        NgapTestGnb::HandoverRequestInfo info;
        if (request.empty() || !NgapTestGnb::parse_handover_request(request, info)) {
            target_failure = "no decodable HandoverRequest reached the target gNB";
            return;
        }
        const auto ack = target.build_handover_request_acknowledge(
            info.amf_ue_id, kTargetRanUeId, info.pdu_session_ids);
        if (ack.empty()) {
            target_failure = "could not build a HandoverRequestAcknowledge";
            return;
        }
        target.send_raw(ack);
    });
    const auto required =
        source.build_handover_required(ue.amf_ue_id, kUeRanId, kTargetGnbId, kPduSessionId);
    ASSERT_FALSE(required.empty());
    source.send_raw(required);
    const auto command = source.receive_raw();
    target_thread.join();
    ASSERT_TRUE(target_failure.empty()) << target_failure;
    ASSERT_FALSE(command.empty()) << "no HandoverCommand -- the relay did not complete";
    ASSERT_EQ(NgapTestGnb::summarize(command).outcome, NgapTestGnb::Outcome::Successful);

    // Execution: the UE has arrived in the target's cell.
    target.send_raw(target.build_handover_notify(ue.amf_ue_id, kTargetRanUeId, kTargetCellId));
    ASSERT_TRUE(mdf2.wait_for(3, 10s)) << "no AMFLocationUpdate after HandoverNotify";
    std::this_thread::sleep_for(500ms);

    const auto pdus = mdf2.take();
    ASSERT_EQ(pdus.size(), 3u) << "Registration + IdentifierAssociation + exactly one LocationUpdate";
    int location_updates = 0;
    for (const auto& pdu : pdus) {
        const auto decoded = xiri::decode_xiri_payload(pdu.payload);
        ASSERT_TRUE(decoded.has_value()) << decoded.error();
        if (const auto* l = std::get_if<xiri::AmfLocationUpdate>(&decoded->event)) {
            ++location_updates;
            EXPECT_EQ(imsi_digits(l->supi), kTargetImsiDigits);
            ASSERT_TRUE(l->location.user_location.has_value());
            ASSERT_TRUE(l->location.user_location->nr.has_value());
            const auto& nr = *l->location.user_location->nr;
            EXPECT_EQ(nr.ncgi.nr_cell_id, kTargetCellId)
                << "the location is not the target cell from the HandoverNotify";
            EXPECT_EQ(nr.tai.plmn.mcc, "999");
            EXPECT_EQ(nr.tai.plmn.mnc, "70");
        }
    }
    EXPECT_EQ(location_updates, 1);

    mdf2_server.stop();
}
