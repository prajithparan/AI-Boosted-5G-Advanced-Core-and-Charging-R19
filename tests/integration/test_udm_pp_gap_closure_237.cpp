// Drives nrf/udr/udm as real, separate OS processes to exercise UDM's Nudm_PP gap-closure
// (ADR-0237, docs/CAPABILITY_GAP_ANALYSIS.md's own UDM audit): the 11 real ops across the 3 real
// sub-groups ADR-0082 disclosed but did not implement -- PP Data Entry (Create/Delete/Get,
// GET/PUT/DELETE /{ueId}/pp-data-store/{afInstanceId}), 5G VN Group (Create/Delete/Modify/Get,
// /5g-vn-groups/{extGroupId}), 5G MBS Group (Create/Delete/Modify/Get,
// /mbs-group-membership/{extGroupId}). PP Data Entry/Create+Delete+Get are real direct proxies to
// UDR's already-live resources; 5G VN/MBS Group's Modify ops are a real RFC 7396-over-RFC 6902
// translation (GET+merge_patch+PUT against UDR).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

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

} // namespace

TEST(UdmPpGapClosure237Integration, PpDataEntryFullLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/nonexistent", 50))
        << "udr never became reachable";

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-pp");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    const std::string supi = "imsi-999700000000001";
    const std::string af_instance_id = "af-test-1";
    const std::string url =
        "https://127.0.0.1:7780/nudm-pp/v1/" + supi + "/pp-data-store/" + af_instance_id;

    // GET before any provisioning correctly 404s.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404) << resp->body;
    }

    // PUT (Create) real 201, real proxied body. `referenceId` is a real optional `Uint64` field
    // on `PpDataEntry` (TS26510_CommonData_grp.hpp's generated struct) -- `afInstanceId` is only
    // a path param for this resource, NOT a body field (confirmed by direct read of the generated
    // struct, not assumed).
    const json entry_body = {{"referenceId", 42}};
    {
        sbi_core::http2::ClientRequest req;
        req.method = "PUT";
        req.url = url;
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = entry_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 201) << resp->body;
    }

    // PUT again (Update via same op) real 204.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "PUT";
        req.url = url;
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = entry_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 204) << resp->body;
    }

    // GET now real 200 with the real proxied data.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 200) << resp->body;
        const auto result = json::parse(resp->body);
        ASSERT_TRUE(result.contains("referenceId"));
        EXPECT_EQ(result.at("referenceId").get<int>(), 42);
    }

    // DELETE real 204, then a second DELETE real 404.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "DELETE";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 204) << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "DELETE";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 404) << resp->body;
    }
}

// Real schemas confirmed by direct read of TS26510_CommonData_grp.hpp's generated structs and
// TS29503_Nudm_PP.yaml directly, not assumed: N5GVnGroupConfiguration's real WIRE key for its
// nested group data is `5gVnGroupData` (numeric-leading, matching the real spec property name --
// the C++ struct field is prefixed `n5gVnGroupData` only because `5gVnGroupData` isn't a valid
// C++ identifier, codegen's own real, disclosed convention), and that nested `5GVnGroupData`
// requires real `dnn`+`sNssai`; MulticastMbsGroupMemb requires a real `multicastGroupMemb` array
// of Gpsi. Both share a real, optional top-level `afInstanceId` string, used here as the real
// Modify target field (simpler to merge-patch/verify than the nested VN-Group-only dnn field).
void run_group_lifecycle(sbi_core::http2::Client& client,
                         const std::string& token,
                         const std::string& segment,
                         const json& create_body) {
    const std::string ext_group_id = "extgroupid-pptest@example.com";
    const std::string url = "https://127.0.0.1:7780/nudm-pp/v1/" + segment + "/" + ext_group_id;

    // GET before creation correctly 404s.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 404) << segment << ": " << resp->body;
    }

    // Create (PUT) real 201.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "PUT";
        req.url = url;
        req.headers.emplace("content-type", "application/json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = create_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 201) << segment << ": " << resp->body;
    }

    // Get real 200 with the real created data.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        ASSERT_EQ(resp->status, 200) << segment << ": " << resp->body;
        EXPECT_EQ(json::parse(resp->body), create_body) << segment;
    }

    // Modify (PATCH, real RFC 7396 merge-patch semantics) real 204, and the change is genuinely
    // persisted (verified via a follow-up real GET) alongside the original real data.
    {
        const json patch_body = {{"afInstanceId", "af-pp-modified"}};
        sbi_core::http2::ClientRequest req;
        req.method = "PATCH";
        req.url = url;
        req.headers.emplace("content-type", "application/merge-patch+json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = patch_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 204) << segment << ": " << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        ASSERT_EQ(resp->status, 200) << segment << ": " << resp->body;
        const auto result = json::parse(resp->body);
        ASSERT_TRUE(result.contains("afInstanceId")) << segment << ": " << resp->body;
        EXPECT_EQ(result.at("afInstanceId").get<std::string>(), "af-pp-modified") << segment;
        for (const auto& [key, value] : create_body.items()) {
            ASSERT_TRUE(result.contains(key)) << segment << " key=" << key << ": " << resp->body;
            EXPECT_EQ(result.at(key), value) << segment << " key=" << key;
        }
    }

    // Modify against an unknown group correctly 404s (real GET-first existence check).
    {
        const json patch_body = {{"afInstanceId", "af-pp-modified"}};
        sbi_core::http2::ClientRequest req;
        req.method = "PATCH";
        req.url =
            "https://127.0.0.1:7780/nudm-pp/v1/" + segment + "/extgroupid-nonexistent@example.com";
        req.headers.emplace("content-type", "application/merge-patch+json");
        req.headers.emplace("authorization", "Bearer " + token);
        req.body = patch_body.dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 404) << segment << ": " << resp->body;
    }

    // Delete real 204, then a second delete real 404.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "DELETE";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 204) << segment << ": " << resp->body;
    }
    {
        sbi_core::http2::ClientRequest req;
        req.method = "DELETE";
        req.url = url;
        req.headers.emplace("authorization", "Bearer " + token);
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value()) << segment;
        EXPECT_EQ(resp->status, 404) << segment << ": " << resp->body;
    }
}

TEST(UdmPpGapClosure237Integration, FiveGVnGroupAndFiveGMbsGroupFullLifecycle) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0) << "failed to fork udr";

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7781/nudr-dr/v2/subscription-data/nonexistent", 50))
        << "udr never became reachable";

    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0) << "failed to fork udm";
    ASSERT_TRUE(wait_reachable(
        client,
        "https://127.0.0.1:7780/nudm-uecm/v1/nonexistent/registrations/amf-3gpp-access",
        50))
        << "udm never became reachable";

    const std::string token = fetch_token(client, "nudm-pp");
    ASSERT_FALSE(token.empty()) << "failed to obtain OAuth2 token from nrf";

    // N5GVnGroupConfiguration: real wire key `5gVnGroupData` (not the C++ field name
    // `n5gVnGroupData`); its own dnn/sNssai are the real required fields.
    const json vn_group_body = {
        {"5gVnGroupData",
         json{{"dnn", "internet"}, {"sNssai", json{{"sst", 1}, {"sd", "000001"}}}}}};
    run_group_lifecycle(client, token, "5g-vn-groups", vn_group_body);

    // MulticastMbsGroupMemb: multicastGroupMemb (real Gpsi array) is the real required field.
    const json mbs_group_body = {{"multicastGroupMemb", json::array({"msisdn-9997000098"})}};
    run_group_lifecycle(client, token, "mbs-group-membership", mbs_group_body);
}
