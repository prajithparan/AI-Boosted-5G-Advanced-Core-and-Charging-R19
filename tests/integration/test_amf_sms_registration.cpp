// ADR-0486 increments 3a, 3b and 4: Registration supporting SMS over NAS (TS 23.502 §4.13.3.1),
// MO SMS (§4.13.3.3) and MT SMS (§4.13.3.6-4.13.3.7) through the real AMF over real NGAP/SCTP +
// NAS, with a real SMSF.
//
// A UE whose RegistrationRequest carries the 5GS update type "SMS requested" bit (TS 24.501
// §9.11.3.9A) makes the AMF call Nsmsf_SMService_Activate (step 5) BEFORE RegistrationAccept, and
// the Accept carries "SMS over NAS allowed" (§9.11.3.6 octet 3 bit 4) only because the SMSF said
// yes (step 9; TS 24.501 §5.5.1.2.4). A UE that does not ask gets "not allowed" and no SMSF
// context. The Accept is deciphered and MAC-verified with keys the UE side derived itself, and the
// SMSF context is checked on the SMSF itself, so neither assertion is the AMF grading its own work.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "ngap_test_gnb.hpp"
#include "sms_core/sms.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"
#include "ue_nas_driver.hpp"

#include <gtest/gtest.h>

namespace {

using nf_test::NgapTestGnb;

constexpr const char* kAmfNgapAddress = "127.0.0.5";
constexpr std::uint16_t kAmfNgapPort = 38412;
constexpr std::uint32_t kGnbId = 0x000044; // clear of the handover/deregistration tests' ids
constexpr const char* kServingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";

constexpr const char* kAusfProbe =
    "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session";
constexpr const char* kUdmProbe = "https://127.0.0.1:7780/nudm-ueau/v1/nonexistent/nonexistent";
constexpr const char* kUdrProbe =
    "https://127.0.0.1:7781/nudr-dr/v1/subscription-data/nonexistent/nonexistent";
constexpr const char* kPcfProbe = "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/none";
constexpr const char* kSmsfProbe = "https://127.0.0.1:7793/nsmsf-sms/v2/ue-contexts/nonexistent";

struct Lab {
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess udr{UDR_PATH};
    nf_test::SpawnedProcess udm{UDM_PATH};
    nf_test::SpawnedProcess ausf{AUSF_PATH};
    nf_test::SpawnedProcess pcf{PCF_PATH};
    nf_test::SpawnedProcess smsf{SMSF_PATH};
    nf_test::SpawnedProcess amf{AMF_PATH};
};

sbi_core::http2::Client make_client() {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

void wait_for_sbi_peers(const std::vector<const char*>& urls, int max_attempts = 200) {
    auto client = make_client();
    for (const char* url : urls) {
        bool reachable = false;
        for (int attempt = 0; attempt < max_attempts && !reachable; ++attempt) {
            sbi_core::http2::ClientRequest req;
            req.method = "GET";
            req.url = url;
            reachable = client.send(req).has_value();
            if (!reachable) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        ASSERT_TRUE(reachable) << "NF never became reachable: " << url;
    }
}

// Registers kTestSupi and returns the deciphered RegistrationAccept (and the UE's keys / AMF id).
void register_ue(NgapTestGnb& gnb,
                 std::uint32_t ran_ue_id,
                 bool sms_requested,
                 std::vector<std::uint8_t>& accept_plain,
                 nf_test::NasKeys* keys_out = nullptr,
                 std::uint64_t* amf_ue_id_out = nullptr) {
    gnb.send_raw(gnb.build_initial_ue_message(
        ran_ue_id, nf_test::build_registration_request(nf_test::kTestSupi, sms_requested)));

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
    const auto keys = nf_test::derive_nas_keys(*challenge, nf_test::kTestSupi, kServingNetworkName);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        challenge_nas.amf_ue_id, ran_ue_id, nf_test::build_security_mode_complete(keys, 0)));

    const auto accept_pdu = gnb.receive_raw();
    ASSERT_FALSE(accept_pdu.empty()) << "no RegistrationAccept";
    NgapTestGnb::DownlinkNas accept_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(accept_pdu, accept_nas));
    const auto plain = nf_test::open_secured_downlink(keys, 1, accept_nas.nas_pdu);
    ASSERT_TRUE(plain.has_value()) << "RegistrationAccept MAC did not verify";
    ASSERT_GE(plain->size(), 5u);
    ASSERT_EQ((*plain)[2], 0x42) << "not a RegistrationAccept";
    accept_plain = *plain;
    if (keys_out != nullptr) {
        *keys_out = keys;
    }
    if (amf_ue_id_out != nullptr) {
        *amf_ue_id_out = challenge_nas.amf_ue_id;
    }
}

std::string nsmsf_token() {
    auto client = make_client();
    sbi_core::http2::ClientRequest token_req;
    token_req.method = "POST";
    token_req.url = "https://127.0.0.1:7777/oauth2/token";
    token_req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    token_req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nsmsf-sms"
        "&targetNfType=SMSF";
    auto token = nf_test::send_token_request(token_req);
    if (!token || token->status != 200) {
        return {};
    }
    return nlohmann::json::parse(token->body).at("access_token").get<std::string>();
}

// The SMS-GMSC's side of Nsmsf_SMService_MtForwardSm (TS 29.540 clause 5.2.2.5.2): POST
// send-mt-sms with SmsData + the CP-DATA(RP-DATA [SMS-DELIVER]) payload.
sbi_core::http2::ClientRequest mt_forward_sm(const std::string& token,
                                             const sms_core::Bytes& payload) {
    sbi_core::multipart::Part json_part;
    json_part.content_type = "application/json";
    json_part.body = R"({"smsPayload":{"contentId":"sms"}})";
    sbi_core::multipart::Part bin_part;
    bin_part.content_type = "application/vnd.3gpp.sms";
    bin_part.content_id = "sms";
    bin_part.body.assign(payload.begin(), payload.end());
    const auto encoded = sbi_core::multipart::encode({json_part, bin_part});
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string("https://127.0.0.1:7793/nsmsf-sms/v2/ue-contexts/") + nf_test::kTestSupi +
              "/send-mt-sms";
    req.headers.emplace("content-type", encoded.content_type_header);
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = encoded.body;
    return req;
}

// CP-DATA (the SMS-GMSC's TI 3) / RP-DATA network->MS (MR 0x11, RP-OA = SC +447785016005) /
// SMS-DELIVER from "46708251358", "hellohello".
sms_core::Bytes gmsc_cp_data() {
    sms_core::RpMessage rp;
    rp.mti = sms_core::RpMti::DataNetworkToMs;
    rp.message_reference = 0x11;
    rp.originator_address = {0x91, 0x44, 0x77, 0x58, 0x10, 0x06, 0x50};
    rp.user_data = sms_core::Bytes{0x04, 0x0B, 0x91, 0x64, 0x07, 0x28, 0x15, 0x53, 0xF8, 0x00,
                                   0x00, 0x62, 0x01, 0x41, 0x21, 0x43, 0x65, 0x00, 0x0A, 0xE8,
                                   0x32, 0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};
    sms_core::CpMessage cp;
    cp.ti_value = 3;
    cp.rpdu = sms_core::encode_rp(rp);
    return sms_core::encode_cp(cp);
}

// The SMSF has a UE SMS context for `supi` iff SMSServiceParameterUpdate (an empty JSON Patch)
// answers 204 rather than 404 -- the SMSF's own state, not the AMF's.
int smsf_context_status(const std::string& supi) {
    auto client = make_client();
    sbi_core::http2::ClientRequest token_req;
    token_req.method = "POST";
    token_req.url = "https://127.0.0.1:7777/oauth2/token";
    token_req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    token_req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nsmsf-sms"
        "&targetNfType=SMSF";
    auto token = nf_test::send_token_request(token_req);
    if (!token || token->status != 200) {
        return -1;
    }
    sbi_core::http2::ClientRequest patch;
    patch.method = "PATCH";
    patch.url = "https://127.0.0.1:7793/nsmsf-sms/v2/ue-contexts/" + supi;
    patch.headers.emplace("content-type", "application/json-patch+json");
    patch.headers.emplace(
        "authorization",
        "Bearer " + nlohmann::json::parse(token->body).at("access_token").get<std::string>());
    patch.body = "[]";
    auto resp = client.send(patch);
    return resp ? resp->status : -1;
}

} // namespace

TEST(AmfSmsRegistration, SmsRequestedActivatesTheSmsfAndAcceptSaysSmsAllowed) {
    Lab lab;
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmsfProbe}));
    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kGnbId));

    std::vector<std::uint8_t> accept;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, 1, /*sms_requested=*/true, accept));
    // 5GS registration result value octet: SMS allowed (bit 4) | 3GPP access (001).
    EXPECT_EQ(accept[4], 0x09);
    EXPECT_EQ(smsf_context_status(nf_test::kTestSupi), 204) << "SMSF holds no UE SMS context";
}

TEST(AmfSmsRegistration, NoSmsRequestedMeansNotAllowedAndNoSmsfContext) {
    Lab lab;
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmsfProbe}));
    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kGnbId));

    std::vector<std::uint8_t> accept;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, 1, /*sms_requested=*/false, accept));
    EXPECT_EQ(accept[4], 0x01);
    EXPECT_EQ(smsf_context_status(nf_test::kTestSupi), 404);
}

// ADR-0486 increment 3b: MO SMS over NAS end to end (TS 23.502 §4.13.3.3): the UE's UL NAS
// TRANSPORT (SMS) reaches the real SMSF via the real AMF, and the SMSF's CP-ACK (step 2c) and
// CP-DATA/RP-ERROR [SMS-SUBMIT-REPORT] (step 6a) come back as DL NAS TRANSPORTs the UE deciphers
// with its own keys; the UE's closing CP-ACK (step 6c) is then accepted with nothing sent back.
TEST(AmfSmsRegistration, MoSmsRoundTripsThroughAmfAndSmsf) {
    Lab lab;
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmsfProbe}));
    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kGnbId));

    constexpr std::uint32_t kRan = 1;
    std::vector<std::uint8_t> accept;
    nf_test::NasKeys keys;
    std::uint64_t amf_ue_id = 0;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, kRan, true, accept, &keys, &amf_ue_id));
    ASSERT_EQ(accept[4], 0x09);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        amf_ue_id, kRan, nf_test::build_registration_complete(keys, /*uplink_count=*/1)));

    // CP-DATA (TI 0, from the TI originator) / RP-DATA (MR 0x2A) / SMS-SUBMIT to "46708251358".
    const sms_core::Bytes submit = {0x11, 0x00, 0x0B, 0x91, 0x64, 0x07, 0x28, 0x15,
                                    0x53, 0xF8, 0x00, 0x00, 0xAA, 0x0A, 0xE8, 0x32,
                                    0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};
    sms_core::RpMessage rp;
    rp.mti = sms_core::RpMti::DataMsToNetwork;
    rp.message_reference = 0x2A;
    rp.destination_address = {0x91, 0x44, 0x77, 0x58, 0x10, 0x06, 0x50};
    rp.user_data = submit;
    sms_core::CpMessage cp;
    cp.rpdu = sms_core::encode_rp(rp);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        amf_ue_id,
        kRan,
        nf_test::build_ul_nas_transport_sms(keys, /*uplink_count=*/2, sms_core::encode_cp(cp))));

    // Step 2c: CP-ACK, DL NAS COUNT 2.
    const auto first = gnb.receive_raw();
    ASSERT_FALSE(first.empty()) << "no downlink after the uplink SMS";
    NgapTestGnb::DownlinkNas dl1;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(first, dl1));
    const auto plain1 = nf_test::open_secured_downlink(keys, 2, dl1.nas_pdu);
    ASSERT_TRUE(plain1.has_value()) << "DL NAS TRANSPORT MAC did not verify";
    const auto sms1 = nf_test::extract_dl_nas_sms(*plain1);
    ASSERT_TRUE(sms1.has_value()) << "not a DL NAS TRANSPORT carrying SMS";
    EXPECT_EQ(*sms1, (sms_core::Bytes{0x89, 0x04})); // CP-ACK, TI flag 1, TI 0

    // Step 6a: CP-DATA / RP-ERROR (no SMS-IWMSC) with the UE's own RP-MR, DL NAS COUNT 3.
    const auto second = gnb.receive_raw();
    ASSERT_FALSE(second.empty()) << "no RP reply";
    NgapTestGnb::DownlinkNas dl2;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(second, dl2));
    const auto plain2 = nf_test::open_secured_downlink(keys, 3, dl2.nas_pdu);
    ASSERT_TRUE(plain2.has_value());
    const auto sms2 = nf_test::extract_dl_nas_sms(*plain2);
    ASSERT_TRUE(sms2.has_value());
    const auto reply_cp = sms_core::decode_cp(*sms2);
    ASSERT_TRUE(reply_cp.has_value());
    EXPECT_EQ(reply_cp->type, sms_core::CpMessageType::Data);
    const auto reply_rp = sms_core::decode_rp(reply_cp->rpdu);
    ASSERT_TRUE(reply_rp.has_value());
    EXPECT_EQ(reply_rp->mti, sms_core::RpMti::ErrorNetworkToMs);
    EXPECT_EQ(reply_rp->message_reference, 0x2A);

    // Step 6c: the UE's CP-ACK closes the transaction; the AMF relays it and nothing comes back.
    gnb.send_raw(gnb.build_uplink_nas_transport(
        amf_ue_id,
        kRan,
        nf_test::build_ul_nas_transport_sms(
            keys, /*uplink_count=*/3, sms_core::encode_cp(sms_core::make_cp_ack(*reply_cp)))));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// ADR-0486 increment 4: MT SMS over NAS end to end (TS 23.502 clauses 4.13.3.6-4.13.3.7, TS 23.540
// clause 5.1.2) with the test as the SMS-GMSC. The real SMSF asks the real AMF for reachability
// (CM-CONNECTED: REACHABLE), the UE deciphers the CP-DATA(RP-DATA) with its own keys, answers
// CP-ACK and CP-DATA(RP-ACK [SMS-DELIVER-REPORT]), gets the SMSF's closing CP-ACK, and the
// SMS-GMSC's MtForwardSm 200 carries the UE's own RP-ACK.
TEST(AmfSmsRegistration, MtSmsRoundTripsThroughSmsfAndAmf) {
    Lab lab;
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmsfProbe}));
    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kGnbId));

    constexpr std::uint32_t kRan = 1;
    std::vector<std::uint8_t> accept;
    nf_test::NasKeys keys;
    std::uint64_t amf_ue_id = 0;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, kRan, true, accept, &keys, &amf_ue_id));
    ASSERT_EQ(accept[4], 0x09);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        amf_ue_id, kRan, nf_test::build_registration_complete(keys, /*uplink_count=*/1)));

    const std::string token = nsmsf_token();
    ASSERT_FALSE(token.empty());
    const auto gmsc_payload = gmsc_cp_data();
    const auto gmsc_cp = sms_core::decode_cp(gmsc_payload);
    ASSERT_TRUE(gmsc_cp.has_value());

    tl::expected<sbi_core::http2::ClientResponse, std::string> mt_resp =
        tl::unexpected(std::string("not sent"));
    std::thread gmsc([&] {
        auto client = make_client();
        mt_resp = client.send(mt_forward_sm(token, gmsc_payload));
    });

    // Steps 5a-5b: CP-DATA on the SMSF's own TI (flag 0), the RP-DATA as the SMS-GMSC sent it.
    const auto first = gnb.receive_raw();
    ASSERT_FALSE(first.empty()) << "the UE never got the MT SMS";
    NgapTestGnb::DownlinkNas dl1;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(first, dl1));
    const auto plain1 = nf_test::open_secured_downlink(keys, 2, dl1.nas_pdu);
    ASSERT_TRUE(plain1.has_value()) << "DL NAS TRANSPORT MAC did not verify";
    const auto sms1 = nf_test::extract_dl_nas_sms(*plain1);
    ASSERT_TRUE(sms1.has_value());
    const auto mt_cp = sms_core::decode_cp(*sms1);
    ASSERT_TRUE(mt_cp.has_value());
    EXPECT_EQ(mt_cp->type, sms_core::CpMessageType::Data);
    EXPECT_FALSE(mt_cp->ti_flag);
    EXPECT_EQ(mt_cp->rpdu, gmsc_cp->rpdu);

    // Steps 5c-6b: the UE's CP-ACK, then CP-DATA(RP-ACK [SMS-DELIVER-REPORT]).
    gnb.send_raw(gnb.build_uplink_nas_transport(
        amf_ue_id,
        kRan,
        nf_test::build_ul_nas_transport_sms(
            keys, /*uplink_count=*/2, sms_core::encode_cp(sms_core::make_cp_ack(*mt_cp)))));
    sms_core::RpMessage rp_ack;
    rp_ack.mti = sms_core::RpMti::AckMsToNetwork;
    rp_ack.message_reference = 0x11;
    rp_ack.user_data = sms_core::Bytes{0x00, 0x00}; // SMS-DELIVER-REPORT, TP-PI 0
    const auto ue_rpdu = sms_core::encode_rp(rp_ack);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        amf_ue_id,
        kRan,
        nf_test::build_ul_nas_transport_sms(
            keys,
            /*uplink_count=*/3,
            sms_core::encode_cp(sms_core::make_cp_data_reply(*mt_cp, ue_rpdu)))));

    // Steps 6c-6d: the SMSF's closing CP-ACK (TI flag back to 0, same TI).
    const auto second = gnb.receive_raw();
    ASSERT_FALSE(second.empty()) << "no closing CP-ACK";
    NgapTestGnb::DownlinkNas dl2;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(second, dl2));
    const auto plain2 = nf_test::open_secured_downlink(keys, 3, dl2.nas_pdu);
    ASSERT_TRUE(plain2.has_value());
    const auto sms2 = nf_test::extract_dl_nas_sms(*plain2);
    ASSERT_TRUE(sms2.has_value());
    const auto close_cp = sms_core::decode_cp(*sms2);
    ASSERT_TRUE(close_cp.has_value());
    EXPECT_EQ(close_cp->type, sms_core::CpMessageType::Ack);
    EXPECT_FALSE(close_cp->ti_flag);
    EXPECT_EQ(close_cp->ti_value, mt_cp->ti_value);

    // Step 7: the delivery report, answering the SMS-GMSC's own TI.
    gmsc.join();
    ASSERT_TRUE(mt_resp.has_value()) << mt_resp.error();
    ASSERT_EQ(mt_resp->status, 200) << mt_resp->body;
    const auto ct = mt_resp->headers.find("content-type");
    ASSERT_NE(ct, mt_resp->headers.end());
    const auto parts = sbi_core::multipart::parse(ct->second, mt_resp->body);
    ASSERT_TRUE(parts.has_value());
    ASSERT_EQ(parts->size(), 2u);
    EXPECT_EQ(nlohmann::json::parse((*parts)[0].body).at("smsPayload").at("contentId"),
              (*parts)[1].content_id.value_or(""));
    const auto report =
        sms_core::decode_cp(sms_core::Bytes((*parts)[1].body.begin(), (*parts)[1].body.end()));
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->type, sms_core::CpMessageType::Data);
    EXPECT_TRUE(report->ti_flag);
    EXPECT_EQ(report->ti_value, 3);
    EXPECT_EQ(report->rpdu, ue_rpdu);
}

// TS 23.540 clause 5.1.5 step 7 / TS 23.502 clause 4.13.3.6 step 4: once the UE's NGAP association
// is gone the AMF reports it UNREACHABLE (no paging in this AMF), and the SMSF answers the SMS-GMSC
// with an error -- 504, no cause (TS 29.540 table 6.1.7.3-1 defines none for this; ADR-0486).
TEST(AmfSmsRegistration, MtSmsToAUeWithNoNgapAssociationFails) {
    Lab lab;
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmsfProbe}));
    {
        NgapTestGnb gnb;
        ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
        ASSERT_TRUE(gnb.ng_setup(kGnbId));
        std::vector<std::uint8_t> accept;
        nf_test::NasKeys keys;
        std::uint64_t amf_ue_id = 0;
        ASSERT_NO_FATAL_FAILURE(register_ue(gnb, 1, true, accept, &keys, &amf_ue_id));
        ASSERT_EQ(accept[4], 0x09);
        // RegistrationComplete: the AMF stores the UE context (its PCF policy association) here,
        // so after the gNB goes the UE is registered but has no NGAP association.
        gnb.send_raw(gnb.build_uplink_nas_transport(
            amf_ue_id, 1, nf_test::build_registration_complete(keys, /*uplink_count=*/1)));
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    } // the gNB's SCTP association closes here
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const std::string token = nsmsf_token();
    ASSERT_FALSE(token.empty());
    auto client = make_client();
    const auto resp = client.send(mt_forward_sm(token, gmsc_cp_data()));
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 504) << resp->body;
    const auto problem = nlohmann::json::parse(resp->body);
    EXPECT_FALSE(problem.contains("cause"));
    EXPECT_NE(problem.at("detail").get<std::string>().find("UNREACHABLE"), std::string::npos);

    // A payload that is not CP-DATA(RP-DATA network->MS) is SMS_PAYLOAD_ERROR.
    const auto bad = client.send(mt_forward_sm(token, sms_core::Bytes{0x09, 0x04}));
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(bad->status, 400);
    EXPECT_EQ(nlohmann::json::parse(bad->body).at("cause"), "SMS_PAYLOAD_ERROR");
}
