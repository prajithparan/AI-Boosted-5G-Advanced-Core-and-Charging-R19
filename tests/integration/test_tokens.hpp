#pragma once

// ADR-0499 phases 4-5: the access tokens tests send, requested the way TS 33.501 13.4.1.1.2 asks
// of any consumer -- by a REGISTERED NF instance whose client certificate names its NF type (the
// NRF's check, phase 2). The test identity is one lab AF: certs/af (CN=af, DNS:af, scripts/
// gen-lab-pki.sh) and a fixed NfInstanceId, registered with tokenless NFRegister before its token
// requests. AF is an NFType value no NF in this repo discovers, so the registration does not
// disturb discovery.
//
// - send_token_request(req): for tests that build their own AccessTokenReq form.
// - attach_test_tokens(client): a request sent without an Authorization header gets the token for
//   its API (sbi_core::TokenSource, phase 3). The NEF's northbound APIs (3gpp-*, TS 29.122/29.522)
//   name no producer NF type and mostly no scope; the NEF accepts any NRF token for itself there
//   (decided by the user, 2026-10-17), so they get a token for the NEF with the YAML's scope where
//   one is declared, else the apiName as the (mandatory) scope value -- a label only.

#include "sbi_core/api_catalog.hpp"
#include "sbi_core/http2_client.hpp"
#include "sbi_core/oauth2_client.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace nf_test {

inline constexpr const char* kTestAfInstanceId = "6c9e5f3a-1d2b-4c8e-9a10-0000000000af";
inline constexpr const char* kTestNrfBase = "https://127.0.0.1:7777";

inline sbi_core::http2::Client& test_af_client() {
    static sbi_core::http2::Client client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/af/cert.pem",
        .key_path = CERTS_DIR "/af/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
    return client;
}

// NFRegister of an NF instance on the NRF now running (retrying while the NRF starts): 201 when
// new; when it is already registered the PUT is an NFUpdate the NRF refuses without a token
// (ADR-0499), which is equally fine here. For test stand-ins too -- a fake AMF must be registered
// for the NRF to issue a token towards AMFs at all (ADR-0499 phase 2).
inline void register_test_nf(const std::string& nf_instance_id, const std::string& nf_type) {
    sbi_core::http2::ClientRequest put;
    put.method = "PUT";
    put.url = std::string(kTestNrfBase) + "/nnrf-nfm/v1/nf-instances/" + nf_instance_id;
    put.headers.emplace("content-type", "application/json");
    put.body = R"({"nfInstanceId":")" + nf_instance_id + R"(","nfType":")" + nf_type +
               R"(","nfStatus":"REGISTERED","ipv4Addresses":["127.0.0.1"]})";
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (test_af_client().send(put).has_value()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

inline void register_test_af() {
    register_test_nf(kTestAfInstanceId, "AF");
}

// Sends an AccessTokenReq form as the registered test AF (registering it first).
inline tl::expected<sbi_core::http2::ClientResponse, std::string>
send_token_request(const sbi_core::http2::ClientRequest& req) {
    register_test_af();
    return test_af_client().send(req);
}

// Sends an AccessTokenReq form as another instance of the lab AF (for tests that tell consumers
// apart by the token's sub): registers `nf_instance_id` as an AF first.
inline tl::expected<sbi_core::http2::ClientResponse, std::string>
send_token_request_as(const sbi_core::http2::ClientRequest& req,
                      const std::string& nf_instance_id) {
    register_test_nf(nf_instance_id, "AF");
    return test_af_client().send(req);
}

inline void attach_test_tokens(sbi_core::http2::Client& client) {
    static const std::string kTokenUrl = std::string(kTestNrfBase) + "/oauth2/token";
    // No back-off: a test's first request may race the NRF's start.
    static sbi_core::TokenSource tokens(
        test_af_client(), kTokenUrl, kTestAfInstanceId, std::chrono::seconds(0));
    static std::mutex northbound_mutex;
    static std::map<std::string, std::unique_ptr<sbi_core::OAuth2Client>> northbound;
    client.set_token_source([](const std::string& url) -> std::optional<std::string> {
        const auto api = sbi_core::api_catalog::api_name_of(url);
        const auto info = sbi_core::api_catalog::find(api);
        const bool northbound_api = api.rfind("3gpp-", 0) == 0;
        if (!northbound_api && (!info || info->nf_type.empty() || info->scope.empty())) {
            return std::nullopt; // no token is due
        }
        if (!northbound_api) {
            if (auto token = tokens.token_for(url)) {
                return token;
            }
            register_test_af(); // refused: not registered (yet, or any more) -- then retry, while
            // the producer may itself still be registering (the NRF issues no token towards an NF
            // type nothing has registered)
            for (int attempt = 0; attempt < 30; ++attempt) {
                if (auto token = tokens.token_for(url)) {
                    return token;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return std::nullopt;
        }
        sbi_core::OAuth2Client* oauth = nullptr;
        {
            std::lock_guard lock(northbound_mutex);
            auto& slot = northbound[api];
            if (!slot) {
                // The YAML's scope where it declares one (3gpp-akma: nnef-akma:gpsi-access).
                const std::string scope =
                    info && !info->scope.empty() ? std::string(info->scope) : api;
                slot = std::make_unique<sbi_core::OAuth2Client>(
                    test_af_client(), kTokenUrl, kTestAfInstanceId, scope, "NEF");
            }
            oauth = slot.get();
        }
        auto token = oauth->get_bearer_token();
        if (!token) {
            register_test_af();
            for (int attempt = 0; attempt < 30 && !token; ++attempt) {
                token = oauth->get_bearer_token();
                if (!token) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        return token ? std::optional<std::string>(*token) : std::nullopt;
    });
}

} // namespace nf_test
