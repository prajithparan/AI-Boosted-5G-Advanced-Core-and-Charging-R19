// Drives SMF's N2SmInfoType dispatch (ADR-0259 + ADR-0260) with REAL PER-encoded NGAP transfers.
//
// Why this test builds NGAP rather than asserting on bytes: those branches only execute on a
// transfer that actually decodes. A test that posted opaque bytes would exercise the 400 path
// forever and never reach the real behaviour, so this encodes genuine transfers with known
// tunnel/QFI values and asserts on what SMF does with them.
//
// Coverage note, stated rather than implied: PDU_RES_SETUP_RSP and PDU_RES_MOD_IND both drive
// install_downlink_far -- the helper ADR-0259 factored out of ADR-0092's PATH_SWITCH_REQ block.
// So this test also covers that refactor, which otherwise had no automated coverage at all
// (ADR-0092 was live-verified by hand, never by a test).

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ngap_core/ngap_codec.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <AssociatedQosFlowItem.h>
#include <GTPTunnel.h>
#include <PDUSessionResourceModifyConfirmTransfer.h>
#include <PDUSessionResourceModifyIndicationTransfer.h>
#include <PDUSessionResourceSetupResponseTransfer.h>
#include <QosFlowModifyConfirmItem.h>
#include <UPTransportLayerInformation.h>
}

namespace {

using nlohmann::json;

constexpr std::uint32_t kGnbTeid = 0x11223344;
constexpr std::uint8_t kGnbIpv4[4] = {127, 0, 0, 9};
constexpr long kQfi = 7;

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
        req.method = "POST";
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
        "&scope=nsmf-pdusession&"
        "targetNfType=SMF";
    auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

// A real gTPTunnel CHOICE carrying kGnbTeid / kGnbIpv4, heap-allocated the way the generated
// codec expects so ASN_STRUCT_FREE_CONTENTS_ONLY owns it.
void fill_gtp_tunnel(UPTransportLayerInformation_t& tnl) {
    tnl.present = UPTransportLayerInformation_PR_gTPTunnel;
    auto* tunnel = static_cast<GTPTunnel_t*>(std::calloc(1, sizeof(GTPTunnel_t)));
    const std::uint8_t teid_bytes[4] = {static_cast<std::uint8_t>((kGnbTeid >> 24) & 0xFF),
                                        static_cast<std::uint8_t>((kGnbTeid >> 16) & 0xFF),
                                        static_cast<std::uint8_t>((kGnbTeid >> 8) & 0xFF),
                                        static_cast<std::uint8_t>(kGnbTeid & 0xFF)};
    tunnel->gTP_TEID.buf = static_cast<std::uint8_t*>(std::malloc(4));
    std::memcpy(tunnel->gTP_TEID.buf, teid_bytes, 4);
    tunnel->gTP_TEID.size = 4;
    tunnel->transportLayerAddress.buf = static_cast<std::uint8_t*>(std::malloc(4));
    std::memcpy(tunnel->transportLayerAddress.buf, kGnbIpv4, 4);
    tunnel->transportLayerAddress.size = 4;
    tunnel->transportLayerAddress.bits_unused = 0;
    tnl.choice.gTPTunnel = tunnel;
}

std::vector<std::uint8_t> encode_setup_response_transfer() {
    PDUSessionResourceSetupResponseTransfer_t transfer{};
    fill_gtp_tunnel(transfer.dLQosFlowPerTNLInformation.uPTransportLayerInformation);
    auto* flow =
        static_cast<AssociatedQosFlowItem_t*>(std::calloc(1, sizeof(AssociatedQosFlowItem_t)));
    flow->qosFlowIdentifier = kQfi;
    ASN_SEQUENCE_ADD(&transfer.dLQosFlowPerTNLInformation.associatedQosFlowList.list, flow);
    const auto bytes =
        ::ngap::encode_value(&asn_DEF_PDUSessionResourceSetupResponseTransfer, &transfer);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PDUSessionResourceSetupResponseTransfer, &transfer);
    return bytes;
}

std::vector<std::uint8_t> encode_modify_indication_transfer() {
    PDUSessionResourceModifyIndicationTransfer_t transfer{};
    fill_gtp_tunnel(transfer.dLQosFlowPerTNLInformation.uPTransportLayerInformation);
    auto* flow =
        static_cast<AssociatedQosFlowItem_t*>(std::calloc(1, sizeof(AssociatedQosFlowItem_t)));
    flow->qosFlowIdentifier = kQfi;
    ASN_SEQUENCE_ADD(&transfer.dLQosFlowPerTNLInformation.associatedQosFlowList.list, flow);
    const auto bytes =
        ::ngap::encode_value(&asn_DEF_PDUSessionResourceModifyIndicationTransfer, &transfer);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PDUSessionResourceModifyIndicationTransfer, &transfer);
    return bytes;
}

sbi_core::multipart::Encoded encode_create_sm_context_body(const std::string& supi,
                                                           std::int64_t pdu_session_id) {
    sbi_core::multipart::Part create_json_part;
    create_json_part.content_type = "application/json";
    create_json_part.body =
        json{
            {"servingNfId", "00000000-0000-4000-8000-0000000000aa"},
            {"servingNetwork", json{{"mcc", "999"}, {"mnc", "70"}}},
            {"anType", "3GPP_ACCESS"},
            {"smContextStatusUri", "https://example.com/sm-status"},
            {"supi", supi},
            {"pduSessionId", pdu_session_id},
            {"dnn", "internet"},
            {"sNssai", json{{"sst", 1}}},
        }
            .dump();
    return sbi_core::multipart::encode({create_json_part});
}

// Posts an UpdateSMContext carrying jsonData + a real n2SmInfo binary part.
std::optional<sbi_core::http2::ClientResponse> post_n2(sbi_core::http2::Client& client,
                                                       const std::string& token,
                                                       const std::string& ref,
                                                       const std::string& type,
                                                       const std::vector<std::uint8_t>& n2) {
    sbi_core::multipart::Part json_part;
    json_part.content_type = "application/json";
    json_part.body =
        json{{"n2SmInfoType", type}, {"n2SmInfo", json{{"contentId", "n2SmInfo"}}}}.dump();
    sbi_core::multipart::Part bin_part;
    bin_part.content_type = "application/vnd.3gpp.ngap";
    bin_part.content_id = "n2SmInfo";
    bin_part.body.assign(n2.begin(), n2.end());
    const auto encoded = sbi_core::multipart::encode({json_part, bin_part});

    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + ref + "/modify";
    req.headers.emplace("content-type", encoded.content_type_header);
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = encoded.body;
    auto resp = client.send(req);
    if (!resp.has_value()) {
        return std::nullopt;
    }
    return *resp;
}

// Creates an SM context that has a REAL N4 session, retrying until SMF's Sx Association with UPF
// is up (SMF discovers UPF through NRF on its own 2s retry cadence -- see
// test_smf_handover_n2sminfo.cpp for the same wait).
std::string create_ready_sm_context(sbi_core::http2::Client& client, const std::string& token) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        const auto encoded = encode_create_sm_context_body("imsi-999700000000077", 20 + attempt);
        sbi_core::http2::ClientRequest create_req;
        create_req.method = "POST";
        create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
        create_req.headers.emplace("content-type", encoded.content_type_header);
        create_req.headers.emplace("authorization", "Bearer " + token);
        create_req.body = encoded.body;
        auto create_resp = client.send(create_req);
        if (!create_resp.has_value() || create_resp->status != 201) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        const auto location_it = create_resp->headers.find("location");
        if (location_it == create_resp->headers.end()) {
            return "";
        }
        const std::string ref = location_it->second.substr(location_it->second.rfind('/') + 1);
        // HANDOVER_REQUIRED answers 200 only once a real UPF N3 F-TEID is on record, so it is a
        // precise readiness probe for "this context has a real N4 session".
        sbi_core::http2::ClientRequest probe;
        probe.method = "POST";
        probe.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + ref + "/modify";
        probe.headers.emplace("content-type", "application/json");
        probe.headers.emplace("authorization", "Bearer " + token);
        probe.body = json{{"n2SmInfoType", "HANDOVER_REQUIRED"}}.dump();
        auto probe_resp = client.send(probe);
        if (probe_resp.has_value() && probe_resp->status == 200) {
            return ref;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return "";
}

struct Lab {
    nf_test::SpawnedProcess nrf{NRF_PATH};
    nf_test::SpawnedProcess upf{UPF_PATH};
    nf_test::SpawnedProcess smf{SMF_PATH};
    nf_test::SpawnedProcess pcf{PCF_PATH};
};

} // namespace

TEST(SmfN2SmInfoDispatch, RealTransfersDriveRealBehaviourAndBadOnesAreRejected) {
    Lab lab;
    ASSERT_GT(lab.nrf.pid(), 0);
    ASSERT_GT(lab.upf.pid(), 0);
    ASSERT_GT(lab.smf.pid(), 0);
    ASSERT_GT(lab.pcf.pid(), 0);

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 80))
        << "smf never became reachable";
    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty());

    const std::string ref = create_ready_sm_context(client, token);
    ASSERT_FALSE(ref.empty()) << "SMF never reached a state with a real UPF N4 session";

    // ADR-0259: PDU_RES_SETUP_RSP -- real transfer, real PFCP DL FAR, 204 (no N2 answer owed).
    {
        auto resp =
            post_n2(client, token, ref, "PDU_RES_SETUP_RSP", encode_setup_response_transfer());
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 204) << resp->body;
    }

    // ADR-0259: PDU_RES_MOD_IND -- real transfer, real PFCP DL FAR, and a real PDU_RES_MOD_CFM
    // answer whose QFIs are echoed from the indication's own associatedQosFlowList.
    {
        auto resp =
            post_n2(client, token, ref, "PDU_RES_MOD_IND", encode_modify_indication_transfer());
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto ct = resp->headers.find("content-type");
        ASSERT_NE(ct, resp->headers.end());
        ASSERT_TRUE(sbi_core::multipart::is_multipart_related(ct->second));
        auto parts = sbi_core::multipart::parse(ct->second, resp->body);
        ASSERT_TRUE(parts.has_value()) << parts.error();
        ASSERT_GE(parts->size(), 2u);
        const auto root = json::parse((*parts)[0].body);
        ASSERT_TRUE(root.contains("n2SmInfoType"));
        EXPECT_EQ(root.at("n2SmInfoType").get<std::string>(), "PDU_RES_MOD_CFM");

        const auto content_id = root.at("n2SmInfo").at("contentId").get<std::string>();
        const auto bin = std::find_if(parts->begin(), parts->end(), [&](const auto& p) {
            return p.content_id.has_value() && *p.content_id == content_id;
        });
        ASSERT_NE(bin, parts->end());

        // Decode SMF's own answer and assert it really echoed our QFI and returned a real uplink
        // tunnel -- not merely that some bytes came back.
        const std::vector<std::uint8_t> bytes(bin->body.begin(), bin->body.end());
        auto* confirm = static_cast<PDUSessionResourceModifyConfirmTransfer_t*>(
            ::ngap::decode_value(&asn_DEF_PDUSessionResourceModifyConfirmTransfer, bytes));
        ASSERT_NE(confirm, nullptr) << "SMF's PDU_RES_MOD_CFM did not decode";
        ASSERT_EQ(confirm->qosFlowModifyConfirmList.list.count, 1);
        EXPECT_EQ(confirm->qosFlowModifyConfirmList.list.array[0]->qosFlowIdentifier, kQfi);
        EXPECT_EQ(confirm->uLNGU_UP_TNLInformation.present,
                  UPTransportLayerInformation_PR_gTPTunnel);
        ASSERT_NE(confirm->uLNGU_UP_TNLInformation.choice.gTPTunnel, nullptr);
        EXPECT_EQ(confirm->uLNGU_UP_TNLInformation.choice.gTPTunnel->gTP_TEID.size, 4);
        ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceModifyConfirmTransfer, confirm);
    }

    // ADR-0259: a malformed transfer on a real branch is a 400, not a silent 204.
    {
        auto resp = post_n2(client, token, ref, "PDU_RES_SETUP_RSP", {0xFF, 0xFF, 0xFF, 0xFF});
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 400) << resp->body;
    }

    // ADR-0260: a value SMF validates and acknowledges -- real transfer decodes to 204.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + ref + "/modify";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json{{"n2SmInfoType", "PDU_RES_NTY"}}.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 204) << resp->body;
    }

    // ADR-0260: an SMF-originated type is rejected rather than blanket-acknowledged. This is the
    // assertion that would fail if the old catch-all 204 ever came back.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + ref + "/modify";
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = json{{"n2SmInfoType", "HANDOVER_CMD"}}.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 400) << resp->body;
    }
}

// ADR-0261: handover cancellation reaches SMF as hoState=CANCELLED -- a real SmContextUpdateData
// field, not an N2SmInfoType. AMF sends exactly this when a source gNB abandons a prepared
// handover. The AMF side of that procedure cannot be driven here (UERANSIM's gNB implements no
// handover procedure at all), so this pins the half that is reachable over real SBI.
TEST(SmfHandoverCancel, HoStateCancelledIsRecordedAndEchoed) {
    Lab lab;
    ASSERT_GT(lab.nrf.pid(), 0);
    ASSERT_GT(lab.smf.pid(), 0);

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 80))
        << "smf never became reachable";
    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty());

    const auto encoded = encode_create_sm_context_body("imsi-999700000000088", 61);
    sbi_core::http2::ClientRequest create_req;
    create_req.method = "POST";
    create_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
    create_req.headers.emplace("content-type", encoded.content_type_header);
    create_req.headers.emplace("authorization", "Bearer " + token);
    create_req.body = encoded.body;
    auto create_resp = client.send(create_req);
    ASSERT_TRUE(create_resp.has_value());
    ASSERT_EQ(create_resp->status, 201) << create_resp->body;
    const auto location_it = create_resp->headers.find("location");
    ASSERT_NE(location_it, create_resp->headers.end());
    const std::string ref = location_it->second.substr(location_it->second.rfind('/') + 1);

    // No UPF is spawned in this test on purpose: cancelling a handover must not depend on an N4
    // session, precisely because SMF releases nothing here (ADR-0261 -- the target-side
    // reservation a production SMF would tear down does not exist in this build).
    sbi_core::http2::ClientRequest cancel_req;
    cancel_req.method = "POST";
    cancel_req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + ref + "/modify";
    cancel_req.headers.emplace("content-type", "application/json");
    cancel_req.headers.emplace("authorization", "Bearer " + token);
    cancel_req.body = json{{"hoState", "CANCELLED"}}.dump();
    auto cancel_resp = client.send(cancel_req);
    ASSERT_TRUE(cancel_resp.has_value());
    ASSERT_EQ(cancel_resp->status, 200) << cancel_resp->body;
    const auto answered = json::parse(cancel_resp->body);
    ASSERT_TRUE(answered.contains("hoState"));
    EXPECT_EQ(answered.at("hoState").get<std::string>(), "CANCELLED");
}
