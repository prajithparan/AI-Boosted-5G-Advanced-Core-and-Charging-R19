// ADR-0264: the first tests to drive AMF's NGAP handover handlers over a REAL SCTP association.
//
// Everything in the N2 handover chain (ADR-0090/0095/0096/0248/0249/0258/0261) has until now been
// verified by construction and review only. UERANSIM (ADR-0016) cannot help: its gNB implements no
// handover procedure at all. `nf_test::NgapTestGnb` is the real gNB that closes that, and these
// are the first assertions it carries.
//
// Scope, stated rather than implied. These cover the paths reachable WITHOUT a registered UE:
// NGSetup, HandoverRequired for an unknown UE, and HandoverCancel for an unknown UE. The full
// relay (HandoverRequired -> SMF -> HandoverRequest -> target gNB -> HandoverCommand) needs a real
// UE security context in AMF, which means driving NAS registration from this driver -- the next
// increment, deliberately not faked here.
//
// ADR-0265/0266 added that registration, and ADR-0267 carries it through to a real PDU session --
// the last piece of UE state the relay depends on, since AMF asks SMF for each session's real N2
// transfer and skips any session it has no smContextRef for.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/multipart.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ngap_test_gnb.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"
#include "ue_nas_driver.hpp"

#include <gtest/gtest.h>

namespace {

using nf_test::NgapTestGnb;

// config/amf.json's own ngap_bind_address/ngap_bind_port.
constexpr const char* kAmfNgapAddress = "127.0.0.5";
constexpr std::uint16_t kAmfNgapPort = 38412;

constexpr std::uint32_t kSourceGnbId = 0x000011;
constexpr std::uint32_t kTargetGnbId = 0x000022;

// The RAN-UE-NGAP-ID the TARGET gNB allocates for the incoming UE. Its own, not the source's --
// that is exactly what HandoverRequestAcknowledge's own RAN-UE-NGAP-ID IE means.
constexpr std::uint32_t kTargetRanUeId = 7777;

// An AMF-UE-NGAP-ID no registration ever allocated, so the cold lookup through amf_ue_id_index
// deliberately misses.
constexpr std::uint64_t kUnknownAmfUeId = 987654;
constexpr std::uint32_t kRanUeId = 4242;

struct Lab {
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess amf{AMF_PATH};
};

// This lab's own configured DNN and S-NSSAI (simulators/ransim/config/ue.yaml, and the values
// tests/integration/test_smf_pdu_session.cpp already drives SMF with).
constexpr const char* kDnn = "internet";
constexpr std::uint8_t kSst = 1;
constexpr std::uint32_t kSd = 0x000001;

constexpr const char* kServingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";

// Each NF's own TLS listener, from config/*.json. Probed rather than assumed -- see
// wait_for_sbi_peers.
constexpr const char* kAusfProbe =
    "https://127.0.0.1:7782/nausf-auth/v1/ue-authentications/nonexistent/eap-session";
constexpr const char* kUdmProbe = "https://127.0.0.1:7780/nudm-ueau/v1/nonexistent/nonexistent";
constexpr const char* kUdrProbe =
    "https://127.0.0.1:7781/nudr-dr/v1/subscription-data/nonexistent/nonexistent";
constexpr const char* kPcfProbe = "https://127.0.0.1:7783/npcf-am-policy-control/v1/policies/none";
constexpr const char* kSmfProbe =
    "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve";
constexpr const char* kNsacfProbe = "https://127.0.0.1:7797/nnsacf-nsac/v1/slices/ues";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

// Blocks until every NF this test's procedures traverse is answering on its own SBI port.
//
// This is load-bearing, not defensive boilerplate. `NgapTestGnb::connect` waits only for AMF's
// NGAP listener, which is the FIRST thing ready -- AMF's SCTP listener was up 44 ms into one CI
// run while AUSF took 440 ms to start. The gNB then sends its RegistrationRequest, AMF calls an
// AUSF that is not listening yet, logs `AUSF call failed: Could not connect to server`, and
// answers the UE nothing at all. There is no retry: the UE waits for a message that will never
// come. On a fast local machine the NFs win the race and it passes; on a loaded CI runner it does
// not. Probing here is the fix, since nothing in the NAS procedures themselves is retryable.
//
// Any completed HTTP response counts as ready -- a 404 from a real listener is exactly the proof
// wanted. Only a connection failure means "not up yet".
void wait_for_sbi_peers(const std::vector<const char*>& urls, int max_attempts = 200) {
    auto client = make_client();
    for (const char* url : urls) {
        bool reachable = false;
        for (int attempt = 0; attempt < max_attempts && !reachable; ++attempt) {
            sbi_core::http2::ClientRequest req;
            req.method = "GET";
            req.url = url;
            if (client.send(req).has_value()) {
                reachable = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ASSERT_TRUE(reachable) << "NF never became reachable, so a procedure that depends on it "
                                  "would stall with no reply rather than fail: "
                               << url;
    }
}

// The whole registration procedure, from RegistrationRequest to the RegistrationAccept AMF sends
// once it has installed a security context -- ADR-0265's authentication plus ADR-0266's security
// mode procedure, factored out because ADR-0267's PDU session test needs exactly the same prelude.
//
// Assertions live here rather than in the caller so a failure points at the step that broke. The
// substance is unchanged: RES* and the NAS keys are computed from the subscriber's real TS 35.207
// credentials and checked by AUSF/AMF against key material they reached independently, so nothing
// here passes on well-formed framing alone.
struct RegisteredUe {
    std::uint64_t amf_ue_id = 0;
    nf_test::NasKeys keys;
};

void register_ue(NgapTestGnb& gnb, std::uint32_t ran_ue_id, RegisteredUe& out) {
    const auto registration = nf_test::build_registration_request(nf_test::kTestSupi);
    ASSERT_FALSE(registration.empty());
    gnb.send_raw(gnb.build_initial_ue_message(ran_ue_id, registration));

    // AMF must fetch a real authentication vector from AUSF (which fetches from UDM/UDR) before it
    // can send this, so reaching it at all already exercises the whole SBI chain.
    const auto challenge_pdu = gnb.receive_raw();
    ASSERT_FALSE(challenge_pdu.empty()) << "AMF sent nothing after the RegistrationRequest";
    NgapTestGnb::DownlinkNas challenge_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(challenge_pdu, challenge_nas))
        << "expected a DownlinkNASTransport carrying an AuthenticationRequest";
    const auto challenge = nf_test::parse_authentication_request(challenge_nas.nas_pdu);
    ASSERT_TRUE(challenge.has_value()) << "AMF's NAS PDU was not a decodable AuthenticationRequest";

    // Verifies AUTN's MAC against the real credentials before answering -- a wrong vector fails
    // here rather than being answered anyway.
    const auto res_star = nf_test::compute_res_star(*challenge, kServingNetworkName);
    ASSERT_TRUE(res_star.has_value()) << "AUTN did not authenticate the network";

    gnb.send_raw(gnb.build_uplink_nas_transport(
        challenge_nas.amf_ue_id, ran_ue_id, nf_test::build_authentication_response(*res_star)));

    const auto smc_pdu = gnb.receive_raw();
    ASSERT_FALSE(smc_pdu.empty()) << "AMF sent nothing after the AuthenticationResponse -- AUSF "
                                     "rejected RES*, or the exchange stalled";
    NgapTestGnb::DownlinkNas smc_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(smc_pdu, smc_nas));
    // SecurityModeCommand is integrity-protected with a new context: 0x7E, SHT, MAC(4), SEQ(1),
    // then the inner plain message whose type byte is 0x5D (TS 24.501 §8.2.25).
    ASSERT_GE(smc_nas.nas_pdu.size(), 10u);
    ASSERT_EQ(smc_nas.nas_pdu[0], 0x7E);
    ASSERT_NE(smc_nas.nas_pdu[1], 0x00) << "SecurityModeCommand must be security-protected";
    ASSERT_EQ(smc_nas.nas_pdu[9], 0x5D)
        << "expected the inner NAS message to be a SecurityModeCommand";

    // ADR-0266: complete the security mode procedure. AMF verifies this message's MAC with keys it
    // derived independently through AUSF -- so its acceptance is proof both sides reached the same
    // KAMF, not proof this driver can round-trip its own bytes. On a wrong key AMF logs
    // "SecurityModeComplete MAC verification FAILED" and answers nothing.
    out.amf_ue_id = challenge_nas.amf_ue_id;
    out.keys = nf_test::derive_nas_keys(*challenge, nf_test::kTestSupi, kServingNetworkName);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        challenge_nas.amf_ue_id,
        ran_ue_id,
        nf_test::build_security_mode_complete(out.keys, /*uplink_count=*/0)));

    const auto accept_pdu = gnb.receive_raw();
    ASSERT_FALSE(accept_pdu.empty())
        << "AMF answered nothing after SecurityModeComplete -- it rejected the MAC, which means "
           "the UE and AMF did not derive the same KAMF";
    NgapTestGnb::DownlinkNas accept_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(accept_pdu, accept_nas));
    ASSERT_GE(accept_nas.nas_pdu.size(), 8u);
    ASSERT_EQ(accept_nas.nas_pdu[0], 0x7E);
    // RegistrationAccept is integrity protected AND ciphered (security header type 0x02), which is
    // only reachable once AMF has installed the security context this whole exchange establishes.
    ASSERT_EQ(accept_nas.nas_pdu[1], 0x02)
        << "expected a ciphered, integrity-protected RegistrationAccept";

    // The UE deciphers and MAC-verifies it for real. This is the FIRST check in either direction
    // that AMF's own downlink protection is correct: every earlier assertion only read the
    // security header AMF wrote, which a wrong key would not have changed. downlink_count=1 --
    // SecurityModeCommand was 0.
    const auto accept_plain =
        nf_test::open_secured_downlink(out.keys, /*downlink_count=*/1, accept_nas.nas_pdu);
    ASSERT_TRUE(accept_plain.has_value())
        << "RegistrationAccept's MAC did not verify against the UE's own KNASint";
    ASSERT_GE(accept_plain->size(), 3u);
    ASSERT_EQ((*accept_plain)[2], 0x42) << "deciphered downlink was not a RegistrationAccept";
}

// ADR-0267's own sequence, factored out so ADR-0269's relay test can reach the same UE state
// without duplicating it: RegistrationComplete, then a real PDU Session Establishment Request,
// then the Accept SMF builds and AMF delivers back on the same association. Assertions live here
// for the same reason they do in register_ue -- a failure names the step that broke.
//
// The NAS COUNTs are not free parameters: AMF's phase machine expects uplink 1 for
// RegistrationComplete and 2 for the UlNasTransport, and answers a message at the wrong count
// with silence rather than an error.
void establish_pdu_session(NgapTestGnb& gnb,
                           const RegisteredUe& ue,
                           std::uint32_t ran_ue_id,
                           std::uint8_t pdu_session_id,
                           std::uint8_t pti) {
    gnb.send_raw(
        gnb.build_uplink_nas_transport(ue.amf_ue_id,
                                       ran_ue_id,
                                       nf_test::build_registration_complete(ue.keys,
                                                                            /*uplink_count=*/1)));

    gnb.send_raw(gnb.build_uplink_nas_transport(
        ue.amf_ue_id,
        ran_ue_id,
        nf_test::build_pdu_session_establishment_request(ue.keys,
                                                         /*uplink_count=*/2,
                                                         pdu_session_id,
                                                         pti,
                                                         kDnn,
                                                         kSst,
                                                         kSd)));

    // The Accept comes back on this same association, but written from AMF's SBI server thread
    // (SMF calls N1N2MessageTransfer, ADR-0038) rather than as a reply on the NGAP thread.
    const auto accept_pdu = gnb.receive_raw();
    ASSERT_FALSE(accept_pdu.empty())
        << "nothing came back after the PDU Session Establishment Request -- AMF rejected the MAC "
           "or NAS COUNT, SMF never created the SM context, or the N1N2MessageTransfer failed";
    NgapTestGnb::DownlinkNas dl;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(accept_pdu, dl))
        << "expected a DownlinkNASTransport carrying the DlNasTransport with the Accept";

    // downlink_count=2: SecurityModeCommand was 0 and RegistrationAccept 1, which is exactly what
    // AMF registers for this UE (next_downlink_count=2). A desync here fails the MAC check.
    const auto plain = nf_test::open_secured_downlink(ue.keys, /*downlink_count=*/2, dl.nas_pdu);
    ASSERT_TRUE(plain.has_value())
        << "the DlNasTransport's MAC did not verify against the UE's own KNASint";

    const auto n1_sm = nf_test::extract_dl_nas_payload_container(*plain);
    ASSERT_TRUE(n1_sm.has_value())
        << "deciphered downlink was not a DlNasTransport carrying an N1 SM container";

    // The payload is SMF's own 5GSM message (nfs/smf/src/nas_5gsm_codec.cpp): EPD 0x2E, the PDU
    // session ID and PTI echoed back from the request, then message type 0xC2 -- PDU Session
    // Establishment Accept (TS 24.501 §8.3.5). AMF never decoded any of this; it relayed the
    // bytes.
    ASSERT_GE(n1_sm->size(), 4u);
    EXPECT_EQ((*n1_sm)[0], 0x2E) << "not a 5GSM message";
    EXPECT_EQ((*n1_sm)[1], pdu_session_id) << "SMF answered about a different PDU session";
    EXPECT_EQ((*n1_sm)[2], pti) << "TS 24.501 requires the PTI to be echoed back";
    EXPECT_EQ((*n1_sm)[3], 0xC2)
        << "expected a PDU Session Establishment Accept; 0xC3 would be a Reject";
}

} // namespace

TEST(AmfNgapTestGnb, NgSetupOverRealSctpSucceeds) {
    Lab lab;
    ASSERT_GT(lab.nrf.pid(), 0) << "failed to fork nrf";
    ASSERT_GT(lab.amf.pid(), 0) << "failed to fork amf";

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort))
        << "never established an SCTP association with AMF's N2 listener";
    EXPECT_TRUE(gnb.ng_setup(kSourceGnbId))
        << "AMF did not answer a real NGSetupRequest with a real NGSetupResponse";
}

// The first end-to-end exercise of handle_handover_required's real decode path. AMF must recognise
// the message, fail to resolve the UE, and answer the spec-defined rejection -- not drop the
// association, and not stay silent.
TEST(AmfNgapTestGnb, HandoverRequiredForUnknownUeIsRejectedWithPreparationFailure) {
    Lab lab;
    ASSERT_GT(lab.nrf.pid(), 0);
    ASSERT_GT(lab.amf.pid(), 0);

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kSourceGnbId));

    const auto required =
        gnb.build_handover_required(kUnknownAmfUeId, kRanUeId, kTargetGnbId, /*pdu_session_id=*/5);
    ASSERT_FALSE(required.empty()) << "failed to PER-encode a real HandoverRequired";
    gnb.send_raw(required);

    const auto reply = gnb.receive_raw();
    ASSERT_FALSE(reply.empty()) << "AMF closed the association instead of answering";
    const auto summary = NgapTestGnb::summarize(reply);
    EXPECT_EQ(summary.outcome, NgapTestGnb::Outcome::Unsuccessful);
    EXPECT_EQ(summary.procedure_code, NgapTestGnb::kProcHandoverPreparation)
        << "expected an unsuccessfulOutcome for id-HandoverPreparation "
           "(HandoverPreparationFailure)";
}

// ADR-0261's AMF half, over the wire for the first time. Its handler deliberately still
// acknowledges a HandoverCancel whose AMF-UE-NGAP-ID it cannot resolve -- it has nothing to tell
// SMF or a target gNB about, but the source gNB is still owed its answer. That behaviour was a
// written claim until this test.
TEST(AmfNgapTestGnb, HandoverCancelForUnknownUeIsStillAcknowledged) {
    Lab lab;
    ASSERT_GT(lab.nrf.pid(), 0);
    ASSERT_GT(lab.amf.pid(), 0);

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kSourceGnbId));

    const auto cancel = gnb.build_handover_cancel(kUnknownAmfUeId, kRanUeId);
    ASSERT_FALSE(cancel.empty()) << "failed to PER-encode a real HandoverCancel";
    gnb.send_raw(cancel);

    const auto reply = gnb.receive_raw();
    ASSERT_FALSE(reply.empty()) << "AMF closed the association instead of acknowledging";
    const auto summary = NgapTestGnb::summarize(reply);
    EXPECT_EQ(summary.outcome, NgapTestGnb::Outcome::Successful);
    EXPECT_EQ(summary.procedure_code, NgapTestGnb::kProcHandoverCancel)
        << "expected a successfulOutcome for id-HandoverCancel (HandoverCancelAcknowledge)";
}

// ADR-0265/0266: a real UE, registering through authentication and the security mode procedure
// over real NGAP + NAS.
//
// This is the increment that makes the full handover relay reachable: AMF resolves a handover's UE
// through amf_ue_id_index -> ue_security_contexts, and only a real registration populates those.
//
// What makes this a real test rather than a mirror of AMF's own encoders: RES* is computed here
// from the subscriber's TS 35.207 credentials and checked by AUSF against what UDM/UDR
// independently hold, and SecurityModeComplete's MAC is checked by AMF against a KAMF it reached
// through AUSF without ever seeing the UE's CK/IK. Well-formed NAS framing does not get you past
// either -- only correct cryptography does.
TEST(AmfNgapTestGnb, RegistrationCompletesAndAmfInstallsASecurityContext) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0);
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0);

    // Authentication traverses AMF -> AUSF -> UDM -> UDR. AMF's NGAP listener is ready long
    // before those are, and AMF does not retry a failed AUSF call -- see wait_for_sbi_peers.
    ASSERT_NO_FATAL_FAILURE(wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe}));

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kSourceGnbId));

    RegisteredUe ue;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, /*ran_ue_id=*/1, ue));
    EXPECT_GT(ue.amf_ue_id, 0u) << "AMF never allocated an AMF-UE-NGAP-ID for this UE";
}

// ADR-0267: the same UE carried on to a real PDU session -- RegistrationComplete, then a PDU
// Session Establishment Request, then the Accept SMF builds and delivers back through AMF.
//
// Why this is the piece the handover relay was waiting on. AMF prepares a handover per PDU session
// by asking SMF for that session's real N2 transfer, keyed on an smContextRef it only holds if it
// really created an SM context for the UE (ADR-0249/0258). A session it has no ref for is SKIPPED,
// not filled with a fabricated tunnel -- so a UE with no PDU session can never get past
// HandoverPreparationFailure, no matter how correct the rest of the chain is.
//
// What is genuinely being checked, beyond framing. The uplink messages are MAC'd with the UE's own
// KNASint at the exact NAS COUNTs AMF's phase machine expects (RegistrationComplete=1,
// UlNasTransport=2), so a count or key that disagrees is rejected in silence rather than answered.
// The downlink Accept is deciphered and MAC-verified here against those same keys, and its content
// is a 5GSM message SMF -- a separate process, which never saw this driver's key material --
// encoded from PCF's real policy decision. AMF is opaque to that content by design; it only
// carries the bytes.
//
// This test spans the widest set of real processes in the suite: NRF, UDR, UDM, AUSF, AMF, PCF and
// SMF, over real NGAP/SCTP for N1/N2 and real TLS 1.3 + mTLS HTTP/2 for every SBI hop between
// them.
TEST(AmfNgapTestGnb, RegisteredUeEstablishesARealPduSession) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0);
    // SMF reaches PCF for the SM policy decision that fixes the session AMBR and 5QI the Accept
    // carries, and calls back into AMF's Namf_Communication N1N2MessageTransfer to deliver it.
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0);
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0);
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0);

    // Every NF this test's two procedures traverse: AMF -> AUSF -> UDM -> UDR for authentication,
    // AMF -> PCF for the AM policy association, AMF -> SMF -> PCF for the PDU session. None of
    // those calls is retried, so a listener that is not up yet costs the test a reply that never
    // arrives -- see wait_for_sbi_peers.
    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmfProbe}));

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kSourceGnbId));

    constexpr std::uint32_t kUeRanId = 1;
    RegisteredUe ue;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, kUeRanId, ue));

    // RegistrationComplete, then the PDU session -- see establish_pdu_session, which carries this
    // test's own assertions and is shared with ADR-0269's relay test rather than copied into it.
    ASSERT_NO_FATAL_FAILURE(
        establish_pdu_session(gnb, ue, kUeRanId, /*pdu_session_id=*/5, /*pti=*/1));
}

namespace {

using nlohmann::json;

std::string fetch_smf_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=nsmf-pdusession&"
        "targetNfType=SMF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

// The same minimal CreateSmContext body tests/integration/test_smf_handover_n2sminfo.cpp builds.
// Duplicated rather than shared: each integration test in this suite is a standalone translation
// unit and the file already duplicates make_client/wait_for_* for the same reason.
sbi_core::multipart::Encoded encode_create_sm_context_body(const std::string& supi,
                                                           std::int64_t pdu_session_id) {
    sbi_core::multipart::Part part;
    part.content_type = "application/json";
    part.body =
        json{
            {"servingNfId", "00000000-0000-4000-8000-0000000000aa"},
            {"servingNetwork", json{{"mcc", "999"}, {"mnc", "70"}}},
            {"anType", "3GPP_ACCESS"},
            {"smContextStatusUri", "https://example.com/sm-status"},
            {"supi", supi},
            {"pduSessionId", pdu_session_id},
            {"dnn", kDnn},
            {"sNssai", json{{"sst", kSst}}},
        }
            .dump();
    return sbi_core::multipart::encode({part});
}

// Blocks until SMF can actually answer HANDOVER_REQUIRED with a real transfer.
//
// Load-bearing, and the trap ADR-0267 recorded as the second of three. SMF only holds a real N3
// uplink F-TEID once it has discovered UPF through NRF and completed a PFCP Sx Association -- its
// own retry loop, on a 2 s cadence -- and it establishes the N4 session INLINE during
// CreateSmContext rather than coming back to the session later. A PDU session created before that
// association exists therefore carries no tunnel forever, HANDOVER_REQUIRED correctly answers 500
// rather than fabricating one, AMF skips the only session it has, and the relay ends in
// HandoverPreparationFailure that reads exactly like an AMF bug.
//
// This gates on the real thing rather than sleeping a guessed interval: a throwaway SUPI's own
// session is created and probed until SMF answers 200. It must run BEFORE the UE establishes its
// session, not after.
void wait_for_upf_sx_association(int max_attempts = 40) {
    auto client = make_client();
    const std::string token = fetch_smf_token(client);
    ASSERT_FALSE(token.empty()) << "failed to obtain an OAuth2 token from NRF for SMF";

    // Deliberately not kTestSupi: this probe session must not collide with the UE's own.
    const std::string supi = "imsi-999700000000042";
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
        ASSERT_NE(location, create_resp->headers.end())
            << "TS 29.502 requires Location on the 201 -- AMF derives its smContextRef from it";
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
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    FAIL() << "SMF never answered HANDOVER_REQUIRED with a real transfer, so no PDU session in "
              "this lab can be handed over. Last answer: "
           << last_failure;
}

// What the target gNB's own thread did, reported back to the test body. A thread cannot carry a
// gtest fatal assertion out of itself, so it records instead and the body asserts.
struct TargetGnbOutcome {
    bool received_request = false;
    bool sent_acknowledge = false;
    NgapTestGnb::HandoverRequestInfo request;
    std::string failure;
};

} // namespace

// ADR-0269: the whole N2 handover relay, end to end, over two real SCTP associations.
//
// This is the assertion every ADR from ADR-0090 through ADR-0261 was written towards and none
// could make. The chain is: a registered UE with a real PDU session sends HandoverRequired from
// the SOURCE gNB; AMF resolves the UE, asks SMF for that session's real N2 transfer
// (n2SmInfoType=HANDOVER_REQUIRED, ADR-0258), builds a real HandoverRequest, sends it to the
// TARGET gNB over that gNB's own separate association, waits for the target's
// HandoverRequestAcknowledge, and answers the source with a real HandoverCommand.
//
// Why it needs both gNBs to be real. AMF keys the target association on the PER-encoded
// GlobalGNB-ID bytes captured from that gNB's own NGSetupRequest, and resolves the target of a
// handover by re-encoding the TargetID the source sent. A test gNB that did not really NGSetup
// under kTargetGnbId would leave AMF with nowhere to send the HandoverRequest, and the failure
// would be indistinguishable from every other failure in this chain -- all of them end in
// HandoverPreparationFailure on the source association.
//
// The three traps ADR-0267 recorded, all of which produce that same symptom, and what handles
// each here: (1) handle_handover_required BLOCKS the source association while awaiting the
// target's reply, so the target must be driven on its own thread -- it is; (2) spawning UPF is not
// enough, the Sx association must exist BEFORE the UE's session is created -- wait_for_upf_sx_-
// association gates on it; (3) AMF's stores live in a shared, long-lived Redis across runs, so
// this test establishes its own session in its own body rather than leaning on ADR-0267's test
// having run first.
TEST(AmfNgapTestGnb, FullN2HandoverRelayThroughExecutionAndSourceRelease) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0);
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0);
    // UPF is what makes this different from ADR-0267's test: without a real PFCP session there is
    // no real N3 uplink F-TEID, and SMF answers HANDOVER_REQUIRED with 500 rather than a
    // fabricated tunnel -- correctly, which is exactly why the relay could not be reached before.
    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0);
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0);
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0);

    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmfProbe}));
    ASSERT_NO_FATAL_FAILURE(wait_for_upf_sx_association());

    NgapTestGnb source;
    ASSERT_TRUE(source.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(source.ng_setup(kSourceGnbId));

    // A second, genuinely separate association -- AMF holds one thread per accepted association
    // (ADR-0096), and this is the first test to make it hold two at once.
    NgapTestGnb target;
    ASSERT_TRUE(target.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(target.ng_setup(kTargetGnbId))
        << "the target gNB never completed NGSetup, so AMF has no association to relay to";

    constexpr std::uint32_t kUeRanId = 1;
    constexpr std::uint8_t kPduSessionId = 5;
    RegisteredUe ue;
    ASSERT_NO_FATAL_FAILURE(register_ue(source, kUeRanId, ue));
    ASSERT_NO_FATAL_FAILURE(establish_pdu_session(source, ue, kUeRanId, kPduSessionId, /*pti=*/1));

    // Started before HandoverRequired is sent, because AMF sends the HandoverRequest and blocks
    // for the answer within that one call. Its receive is bounded by the driver's own socket
    // timeout, so a HandoverRequest that never arrives fails this test with a message rather than
    // hanging until ctest kills the binary.
    TargetGnbOutcome target_outcome;
    std::thread target_thread([&] {
        const auto request = target.receive_raw();
        if (request.empty()) {
            target_outcome.failure = "no HandoverRequest ever arrived on the target association";
            return;
        }
        if (!NgapTestGnb::parse_handover_request(request, target_outcome.request)) {
            target_outcome.failure = "the PDU AMF sent the target was not a decodable "
                                     "HandoverRequest carrying an AMF-UE-NGAP-ID";
            return;
        }
        target_outcome.received_request = true;
        const auto ack =
            target.build_handover_request_acknowledge(target_outcome.request.amf_ue_id,
                                                      kTargetRanUeId,
                                                      target_outcome.request.pdu_session_ids);
        if (ack.empty()) {
            target_outcome.failure = "could not build a HandoverRequestAcknowledge -- AMF prepared "
                                     "no PDU session, so there was nothing to admit";
            return;
        }
        target.send_raw(ack);
        target_outcome.sent_acknowledge = true;
    });

    const auto required =
        source.build_handover_required(ue.amf_ue_id, kUeRanId, kTargetGnbId, kPduSessionId);
    ASSERT_FALSE(required.empty());
    source.send_raw(required);

    const auto reply = source.receive_raw();
    target_thread.join();

    // The target's own report first: it names which step broke, where the source's reply can only
    // say "not a HandoverCommand".
    ASSERT_TRUE(target_outcome.failure.empty()) << target_outcome.failure;
    ASSERT_TRUE(target_outcome.received_request);
    EXPECT_EQ(target_outcome.request.amf_ue_id, ue.amf_ue_id)
        << "AMF addressed the HandoverRequest to a different UE than the one that registered";
    ASSERT_EQ(target_outcome.request.pdu_session_ids.size(), 1u)
        << "AMF prepared a different number of PDU sessions than the one this UE has -- a session "
           "SMF could not answer for is SKIPPED, never fabricated (ADR-0258)";
    EXPECT_EQ(target_outcome.request.pdu_session_ids[0], kPduSessionId);
    EXPECT_TRUE(target_outcome.sent_acknowledge);

    ASSERT_FALSE(reply.empty()) << "AMF answered the source gNB nothing at all";
    const auto summary = NgapTestGnb::summarize(reply);
    EXPECT_EQ(summary.outcome, NgapTestGnb::Outcome::Successful)
        << "an unsuccessfulOutcome here is HandoverPreparationFailure -- AMF could not complete "
           "the relay; its own log names which step";
    EXPECT_EQ(summary.procedure_code, NgapTestGnb::kProcHandoverPreparation)
        << "expected a successfulOutcome for id-HandoverPreparation (HandoverCommand)";

    // ADR-0270: the HandoverCommand must name the sessions whose downlink SMF really switched.
    //
    // This is what stops the relay from passing while the user plane is left behind. AMF fills
    // PDUSessionResourceHandoverList only for sessions SMF answered HANDOVER_REQ_ACK for -- which
    // it can only do after a real PFCP Session Modification repointing UPF's downlink FAR at the
    // target's tunnel. An empty list here means the NGAP relay completed while UPF still sends
    // downlink to the SOURCE gNB, which is exactly the state ADR-0269 found and disclosed and
    // ADR-0270 fixed. Without this assertion that regression passes silently.
    std::vector<std::uint8_t> switched;
    ASSERT_TRUE(NgapTestGnb::parse_handover_command(reply, switched));
    ASSERT_EQ(switched.size(), 1u)
        << "the HandoverCommand carried no PDUSessionResourceHandoverList -- AMF answered the "
           "source gNB without telling SMF where the target wants downlink, so UPF is still "
           "pointed at the source";
    EXPECT_EQ(switched[0], kPduSessionId);

    // --- ADR-0271: the execution phase (TS 23.502 §4.9.1.3.3) ---
    //
    // The UE arrives at the target, which says so with HandoverNotify. AMF then owes two things:
    // tell SMF the handover completed (hoState=COMPLETED, per PDU session) and release what the
    // SOURCE gNB is still holding. It does them in that order, so the release arriving here is
    // also evidence the SMF loop ran ahead of it -- SMF's own log
    // ("handover COMPLETED for smContextRef ... recorded") is what confirms SMF answered, and this
    // test deliberately does not assert on SMF's internal state, which it has no read path to.
    target.send_raw(target.build_handover_notify(ue.amf_ue_id, kTargetRanUeId));

    const auto release = source.receive_raw();
    ASSERT_FALSE(release.empty())
        << "the source gNB was never released after the UE arrived at the target -- it would hold "
           "that UE's resources forever";
    const auto release_summary = NgapTestGnb::summarize(release);
    EXPECT_EQ(release_summary.outcome, NgapTestGnb::Outcome::Initiating);
    EXPECT_EQ(release_summary.procedure_code, NgapTestGnb::kProcUeContextRelease)
        << "expected an AMF-initiated UEContextReleaseCommand to the source gNB";
}

// ADR-0272: the reject branch. The target gNB refuses the handover, and AMF must relay that
// refusal to the source gNB as a HandoverPreparationFailure carrying the TARGET's own cause.
//
// Why this needed its own test rather than being assumed from the accept path: that relay is a
// distinct branch of handle_handover_required with its own decode (HandoverFailure's Cause IE is
// pulled out and copied into the HandoverPreparationFailure), and every OTHER failure in the whole
// chain also ends in HandoverPreparationFailure -- an unregistered target, a decode failure, a
// timeout, no preparable session. So "the source got a HandoverPreparationFailure" proves almost
// nothing on its own. Asserting the CAUSE is what separates "AMF relayed the target's refusal"
// from "AMF gave up for one of six other reasons and happened to look the same".
//
// The setup is the accept path's, unchanged, right up to the target's answer -- the same real UE,
// real PDU session and real UPF Sx association -- because AMF only ever contacts the target if it
// really prepared at least one session.
TEST(AmfNgapTestGnb, TargetGnbRefusalIsRelayedToTheSourceWithItsOwnCause) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0);
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0);
    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0);
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0);
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0);

    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmfProbe}));
    ASSERT_NO_FATAL_FAILURE(wait_for_upf_sx_association());

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
    ASSERT_NO_FATAL_FAILURE(establish_pdu_session(source, ue, kUeRanId, kPduSessionId, /*pti=*/1));

    // Same threading rule as the accept path: AMF blocks the source association while it waits
    // for this answer, so the refusal has to be sent from the target's own thread.
    TargetGnbOutcome target_outcome;
    std::thread target_thread([&] {
        const auto request = target.receive_raw();
        if (request.empty()) {
            target_outcome.failure = "no HandoverRequest ever arrived on the target association";
            return;
        }
        if (!NgapTestGnb::parse_handover_request(request, target_outcome.request)) {
            target_outcome.failure = "the PDU AMF sent the target was not a decodable "
                                     "HandoverRequest";
            return;
        }
        target_outcome.received_request = true;
        target.send_raw(target.build_handover_failure(target_outcome.request.amf_ue_id));
        target_outcome.sent_acknowledge = true; // a refusal, but the same "answered" flag
    });

    const auto required =
        source.build_handover_required(ue.amf_ue_id, kUeRanId, kTargetGnbId, kPduSessionId);
    ASSERT_FALSE(required.empty());
    source.send_raw(required);

    const auto reply = source.receive_raw();
    target_thread.join();

    ASSERT_TRUE(target_outcome.failure.empty()) << target_outcome.failure;
    ASSERT_TRUE(target_outcome.received_request)
        << "AMF never asked the target at all, so nothing here tests the refusal path";

    ASSERT_FALSE(reply.empty()) << "AMF answered the source gNB nothing at all";
    const auto summary = NgapTestGnb::summarize(reply);
    EXPECT_EQ(summary.outcome, NgapTestGnb::Outcome::Unsuccessful);
    EXPECT_EQ(summary.procedure_code, NgapTestGnb::kProcHandoverPreparation)
        << "expected an unsuccessfulOutcome for id-HandoverPreparation "
           "(HandoverPreparationFailure)";

    // The assertion that actually distinguishes this branch from the other five ways the same
    // message gets sent.
    long relayed_cause = -1;
    ASSERT_TRUE(NgapTestGnb::parse_handover_preparation_failure_cause(reply, relayed_cause))
        << "the HandoverPreparationFailure carried no radioNetwork Cause to relay";
    EXPECT_EQ(relayed_cause, NgapTestGnb::kCauseRadioNoResourcesInTargetCell)
        << "AMF substituted its own cause instead of relaying the target gNB's -- the source gNB "
           "is told the wrong reason its handover failed";
}

// ADR-0277: the wiring. A real UE registering through AMF, and a real PDU session through SMF,
// must reach NSACF -- proved by receiving NSACF's own event report rather than by reading a log.
//
// Why the report is the evidence: Nnsacf_NSAC has no operation that returns the CURRENT count
// (QuotaUpdate returns the configured maxima), so a test cannot ask NSACF "how many UEs do you
// hold". What it can do is subscribe to NUM_OF_REGD_UES with a threshold of 1 and let NSACF say
// so itself. The count in that report came from AMF's own call, made during a registration this
// test drove over real NGAP/NAS -- four processes and two protocols away from the assertion.
TEST(AmfNgapTestGnb, RegistrationAndPduSessionReachNsacf) {
    boost::asio::io_context receiver_ioc;
    sbi_core::http2::TlsConfig receiver_tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Server receiver(receiver_ioc, "127.0.0.1", 19997, receiver_tls);

    std::mutex reports_mutex;
    std::vector<json> reports;
    receiver.add_route(
        "POST", "/sac-report", [&reports_mutex, &reports](const sbi_core::http2::Request& req) {
            std::lock_guard<std::mutex> lock(reports_mutex);
            reports.push_back(json::parse(req.body));
            sbi_core::http2::Response resp;
            resp.status = 204;
            return resp;
        });
    receiver.start();
    std::thread receiver_thread([&receiver_ioc] { receiver_ioc.run(); });

    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0);
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0);
    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0);
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0);
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    ASSERT_GT(nsacf.pid(), 0);
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0);

    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmfProbe, kNsacfProbe}));
    ASSERT_NO_FATAL_FAILURE(wait_for_upf_sx_association());

    // Subscribe before the UE registers, threshold 1, on the lab's own S-NSSAI (sst=1, sd=000001)
    // -- the slice AMF reports and the one config/nsacf.json configures.
    {
        auto client = make_client();
        nf_test::attach_test_tokens(client); // ADR-0499: the NSACF enforces tokens
        sbi_core::http2::ClientRequest sub_req;
        sub_req.method = "POST";
        sub_req.url = "https://127.0.0.1:7797/nnsacf-slice-ee/v1/subscriptions";
        sub_req.headers.emplace("content-type", "application/json");
        sub_req.body =
            json{
                {"event",
                 json{{"eventType", "NUM_OF_REGD_UES"},
                      {"eventTrigger", "THRESHOLD"},
                      {"eventFilter", json::array({json{{"sst", 1}, {"sd", "000001"}}})},
                      {"notifThreshold", json{{"numericValNumUes", 1}}}}},
                {"eventNotifyUri", "https://127.0.0.1:19997/sac-report"},
                {"nfId", "00000000-0000-4000-8000-0000000000bb"},
            }
                .dump();
        auto sub_resp = client.send(sub_req);
        ASSERT_TRUE(sub_resp.has_value());
        ASSERT_EQ(sub_resp->status, 201) << sub_resp->body;
    }

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kSourceGnbId));

    constexpr std::uint32_t kUeRanId = 1;
    RegisteredUe ue;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, kUeRanId, ue));
    ASSERT_NO_FATAL_FAILURE(establish_pdu_session(gnb,
                                                  ue,
                                                  kUeRanId,
                                                  /*pdu_session_id=*/5,
                                                  /*pti=*/1));

    for (int attempt = 0; attempt < 60; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(reports_mutex);
            if (!reports.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    receiver_ioc.stop();
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }

    std::lock_guard<std::mutex> lock(reports_mutex);
    ASSERT_FALSE(reports.empty())
        << "NSACF never reported a registered UE -- AMF's Nnsacf_NSAC call during Registration "
           "(TS 23.502 §4.2.2.2.2) did not reach it";
    const auto& report = reports.front().at("report");
    EXPECT_EQ(report.at("eventType").get<std::string>(), "NUM_OF_REGD_UES");
    EXPECT_EQ(report.at("eventFilter").at("sst").get<int>(), 1);
    EXPECT_GE(report.at("sliceStautsInfo").at("reachedNumUes").at("numericValNumUes").get<int>(), 1)
        << "the report must count the UE this test registered";
}

// ADR-0278: NSAC is now ENFORCED, not just reported. A UE whose slice NSACF refuses is sent a real
// RegistrationReject instead of a RegistrationAccept, and is never counted as registered.
//
// The slice is emptied of capacity with NSACF's own LocalNumberUpdate (maxUesNumber: 0) rather
// than by registering 100 UEs -- that operation exists precisely so an operator can change the
// maximum at runtime, so using it here exercises a real path instead of brute force.
//
// The assertion is on the DECIPHERED downlink: message type 0x44 (RegistrationReject, TS 24.501
// §8.2.8) carrying 5GMM cause 0x45 (insufficient resources for slice), where a passing UE would
// have received 0x42. The UE verifies it with its own KNASint, so this also proves AMF protected
// the reject with the security context it had just established rather than sending it in the
// clear.
TEST(AmfNgapTestGnb, UeIsRejectedWhenNsacfRefusesItsSlice) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0);
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0);
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    ASSERT_GT(nsacf.pid(), 0);
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0);

    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kNsacfProbe}));

    // Take the lab slice's UE capacity to zero, through the real operation for it.
    {
        auto client = make_client();
        nf_test::attach_test_tokens(client); // ADR-0499: the NSACF enforces tokens
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7797/nnsacf-nsac/v1/slices/local-configs/update";
        req.headers.emplace("content-type", "application/json");
        req.body =
            json{{"snssai", json{{"sst", kSst}, {"sd", "000001"}}}, {"maxUesNumber", 0}}.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 204) << resp->body;
    }

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kSourceGnbId));

    // The registration exchange, up to SecurityModeComplete -- identical to register_ue's, which
    // cannot be reused here because it asserts on the RegistrationAccept this UE must not get.
    constexpr std::uint32_t kUeRanId = 1;
    const auto registration = nf_test::build_registration_request(nf_test::kTestSupi);
    ASSERT_FALSE(registration.empty());
    gnb.send_raw(gnb.build_initial_ue_message(kUeRanId, registration));

    const auto challenge_pdu = gnb.receive_raw();
    ASSERT_FALSE(challenge_pdu.empty());
    NgapTestGnb::DownlinkNas challenge_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(challenge_pdu, challenge_nas));
    const auto challenge = nf_test::parse_authentication_request(challenge_nas.nas_pdu);
    ASSERT_TRUE(challenge.has_value());
    const auto res_star = nf_test::compute_res_star(*challenge, kServingNetworkName);
    ASSERT_TRUE(res_star.has_value());
    gnb.send_raw(gnb.build_uplink_nas_transport(
        challenge_nas.amf_ue_id, kUeRanId, nf_test::build_authentication_response(*res_star)));

    const auto smc_pdu = gnb.receive_raw();
    ASSERT_FALSE(smc_pdu.empty());
    NgapTestGnb::DownlinkNas smc_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(smc_pdu, smc_nas));
    const auto keys = nf_test::derive_nas_keys(*challenge, nf_test::kTestSupi, kServingNetworkName);
    gnb.send_raw(gnb.build_uplink_nas_transport(
        challenge_nas.amf_ue_id,
        kUeRanId,
        nf_test::build_security_mode_complete(keys, /*uplink_count=*/0)));

    // Where a passing UE gets RegistrationAccept, this one must get RegistrationReject.
    const auto reject_pdu = gnb.receive_raw();
    ASSERT_FALSE(reject_pdu.empty())
        << "AMF answered nothing after SecurityModeComplete -- a refused UE must still be told, "
           "not left waiting";
    NgapTestGnb::DownlinkNas reject_nas;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(reject_pdu, reject_nas));
    ASSERT_GE(reject_nas.nas_pdu.size(), 8u);
    EXPECT_EQ(reject_nas.nas_pdu[1], 0x02)
        << "the reject must be integrity protected AND ciphered with the context just established";

    const auto plain =
        nf_test::open_secured_downlink(keys, /*downlink_count=*/1, reject_nas.nas_pdu);
    ASSERT_TRUE(plain.has_value())
        << "the RegistrationReject's MAC did not verify against the UE's own KNASint";
    ASSERT_GE(plain->size(), 4u);
    EXPECT_EQ((*plain)[2], 0x44)
        << "expected RegistrationReject (0x44); 0x42 would mean the UE was accepted onto a slice "
           "NSACF had refused";
    EXPECT_EQ((*plain)[3], 0x45) << "expected 5GMM cause 0x45, insufficient resources for slice";
}

// ADR-0279: SMF enforces slice admission too, and the UE is told why.
//
// This is the half ADR-0278 named as still open. It needed more than moving a call: SMF's error
// responses used generic ProblemDetails, which has nowhere to carry a NAS message, and AMF
// discarded SMF's error bodies entirely -- so a refused UE would have waited for a message that
// never came. SMF now answers 403 with TS 29.502's SmContextCreateError carrying the reject in
// n1SmMsg, and AMF relays it through the same registry path the Accept uses.
//
// The assertion is the whole point: the UE deciphers a DlNasTransport whose N1 SM container is a
// PDU Session Establishment Reject (0xC3) with 5GSM cause 0x45, where a passing UE gets 0xC2.
TEST(AmfNgapTestGnb, PduSessionIsRejectedWhenNsacfRefusesTheSlice) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    nf_test::SpawnedProcess ausf(AUSF_PATH);
    ASSERT_GT(ausf.pid(), 0);
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0);
    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0);
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0);
    nf_test::SpawnedProcess nsacf(NSACF_PATH);
    ASSERT_GT(nsacf.pid(), 0);
    nf_test::SpawnedProcess amf(AMF_PATH);
    ASSERT_GT(amf.pid(), 0);

    ASSERT_NO_FATAL_FAILURE(
        wait_for_sbi_peers({kUdrProbe, kUdmProbe, kAusfProbe, kPcfProbe, kSmfProbe, kNsacfProbe}));

    // UE capacity stays as configured -- the UE must REGISTER successfully -- while the slice's
    // PDU-session capacity goes to zero. That separation is what makes this test about SMF's half
    // rather than AMF's: the same slice admits the UE and refuses its session.
    {
        auto client = make_client();
        nf_test::attach_test_tokens(client); // ADR-0499: the NSACF enforces tokens
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7797/nnsacf-nsac/v1/slices/local-configs/update";
        req.headers.emplace("content-type", "application/json");
        req.body =
            json{{"snssai", json{{"sst", kSst}, {"sd", "000001"}}}, {"maxPdusNumber", 0}}.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 204) << resp->body;
    }

    NgapTestGnb gnb;
    ASSERT_TRUE(gnb.connect(kAmfNgapAddress, kAmfNgapPort));
    ASSERT_TRUE(gnb.ng_setup(kSourceGnbId));

    constexpr std::uint32_t kUeRanId = 1;
    constexpr std::uint8_t kPduSessionId = 5;
    constexpr std::uint8_t kPti = 1;
    RegisteredUe ue;
    ASSERT_NO_FATAL_FAILURE(register_ue(gnb, kUeRanId, ue));

    gnb.send_raw(gnb.build_uplink_nas_transport(
        ue.amf_ue_id, kUeRanId, nf_test::build_registration_complete(ue.keys, /*uplink_count=*/1)));
    gnb.send_raw(gnb.build_uplink_nas_transport(
        ue.amf_ue_id,
        kUeRanId,
        nf_test::build_pdu_session_establishment_request(ue.keys,
                                                         /*uplink_count=*/2,
                                                         kPduSessionId,
                                                         kPti,
                                                         kDnn,
                                                         kSst,
                                                         kSd)));

    const auto reject_pdu = gnb.receive_raw();
    ASSERT_FALSE(reject_pdu.empty())
        << "nothing came back after the PDU Session Establishment Request -- a refused session "
           "must still be answered, which is exactly what this increment fixed";
    NgapTestGnb::DownlinkNas dl;
    ASSERT_TRUE(NgapTestGnb::extract_downlink_nas(reject_pdu, dl));

    const auto plain = nf_test::open_secured_downlink(ue.keys, /*downlink_count=*/2, dl.nas_pdu);
    ASSERT_TRUE(plain.has_value())
        << "the DlNasTransport carrying the reject did not verify against the UE's own KNASint";
    const auto n1_sm = nf_test::extract_dl_nas_payload_container(*plain);
    ASSERT_TRUE(n1_sm.has_value()) << "deciphered downlink carried no N1 SM container";

    ASSERT_GE(n1_sm->size(), 5u);
    EXPECT_EQ((*n1_sm)[0], 0x2E) << "not a 5GSM message";
    EXPECT_EQ((*n1_sm)[1], kPduSessionId);
    EXPECT_EQ((*n1_sm)[2], kPti) << "TS 24.501 requires the PTI to be echoed on the reject too";
    EXPECT_EQ((*n1_sm)[3], 0xC3)
        << "expected a PDU Session Establishment Reject; 0xC2 would mean the session was "
           "established on a slice NSACF had refused";
    EXPECT_EQ((*n1_sm)[4], 0x45) << "expected 5GSM cause 0x45, insufficient resources for slice";
}
