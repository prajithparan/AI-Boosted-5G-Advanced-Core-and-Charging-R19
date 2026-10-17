// hello-nf: exercises libs/sbi-core's HTTP/2 client, OAuth2 client-credentials flow (now with a
// real signed JWT, verified by the real NRF -- see nfs/nrf), and 3gpp-Sbi-* header handling
// end-to-end over real TLS 1.3 + mTLS. Not a real NF -- no NFType in
// TS29510_Nnrf_NFManagement.yaml's enum fits "hello world", so this registers itself with
// nfType="AMF" as a placeholder (the schema's NFType is an open anyOf [enum, plain string], so any
// string is technically schema-valid; AMF is chosen only because it's next in the Phase 2 build
// order, not because this process implements any AMF behaviour).
//
// Lifecycle: fetch OAuth2 token -> PUT register -> PATCH heartbeat -> DELETE deregister -> exit.
// Exit code 0 iff every step succeeded, so tests/integration can drive this as a subprocess and
// check the exit code as the pass/fail signal.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/otel.hpp"
#include "sbi_core/sbi_headers.hpp"
#include "sbi_core/uuid.hpp"

// docs/DECISIONS.md ADR-0077 -- no hardcoded deployment literal in source.
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "nf_config/nf_config.hpp"

#ifndef CONFIG_DIR
#error "CONFIG_DIR must be defined by CMake (see nfs/hello-nf/CMakeLists.txt)"
#endif

namespace {

using nlohmann::json;

bool wait_for_nrf(sbi_core::http2::Client& client,
                  const std::string& nrf_base,
                  int max_attempts = 20) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/00000000-0000-4000-8000-000000000000";
        auto resp = client.send(req);
        // Any HTTP response (even 404) means the server is up; connection failure means retry.
        if (resp.has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

} // namespace

int main() {
    const auto config = nf_config::load("hello-nf", CONFIG_DIR);
    const auto nrf_base =
        nf_config::require<std::string>(config, "nrf_base_url", "HELLO_NF_NRF_BASE_URL");

    sbi_core::init_logging("hello-nf");
    sbi_core::init_tracing("hello-nf");
    auto tracer = sbi_core::get_tracer();

    const std::string nf_instance_id = sbi_core::generate_uuid_v4();
    spdlog::info("hello-nf: starting, nfInstanceId={}", nf_instance_id);

    sbi_core::http2::TlsConfig tls{
        // ADR-0499 phase 5: the NRF issues tokens only to a client whose certificate names the NF
        // type it registered as (TS 29.510 5.4.2.2.1) -- AMF here, so the lab AMF certificate.
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client http_client(std::move(tls));

    if (!wait_for_nrf(http_client, nrf_base)) {
        spdlog::error("hello-nf: nrf never became reachable at {}", nrf_base);
        return 1;
    }

    sbi_core::OAuth2Client oauth(
        http_client, nrf_base + "/oauth2/token", nf_instance_id, "nnrf-nfm", "NRF");

    tl::expected<std::string, std::string> token;
    {
        auto span = tracer->StartSpan("hello-nf.register");
        auto scope = opentelemetry::trace::Scope(span);

        json profile{
            {"nfInstanceId", nf_instance_id},
            {"nfType", "AMF"},
            {"nfStatus", "REGISTERED"},
            {"ipv4Addresses", json::array({"127.0.0.1"})},
            {"heartBeatTimer", 30},
        };

        sbi_core::http2::ClientRequest put_req;
        put_req.method = "PUT";
        put_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nf_instance_id;
        put_req.headers.emplace("content-type", "application/json");
        put_req.headers.emplace(
            sbi_core::headers::kSenderTimestamp,
            sbi_core::headers::format_sender_timestamp(std::chrono::system_clock::now()));
        put_req.body = profile.dump();

        auto put_resp = http_client.send(put_req);
        if (!put_resp.has_value() || (put_resp->status != 200 && put_resp->status != 201)) {
            const std::string err = put_resp.has_value() ? put_resp->body : put_resp.error();
            spdlog::error("hello-nf: registration failed: {}", err);
            span->SetStatus(opentelemetry::trace::StatusCode::kError, "registration failed");
            span->End();
            return 1;
        }
        spdlog::info("hello-nf: registered (HTTP {})", put_resp->status);

        // ADR-0499 phase 3: NFRegister needs no token (the NFManagement API admits requests
        // without one); the token comes after it, since TS 33.501 13.4.1.1.2 makes registration
        // the pre-requisite of a token.
        token = oauth.get_bearer_token();
        if (!token.has_value()) {
            spdlog::error("hello-nf: OAuth2 token fetch failed: {}", token.error());
            span->SetStatus(opentelemetry::trace::StatusCode::kError, token.error());
            span->End();
            return 1;
        }
        spdlog::info("hello-nf: obtained OAuth2 bearer token");
        span->End();
    }

    {
        auto heartbeat_span = tracer->StartSpan("hello-nf.heartbeat");
        auto heartbeat_scope = opentelemetry::trace::Scope(heartbeat_span);

        sbi_core::http2::ClientRequest patch_req;
        patch_req.method = "PATCH";
        patch_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nf_instance_id;
        patch_req.headers.emplace("content-type", "application/json-patch+json");
        patch_req.headers.emplace("authorization", "Bearer " + *token);
        patch_req.body =
            json::array({json{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}})
                .dump();

        auto patch_resp = http_client.send(patch_req);
        if (!patch_resp.has_value() || patch_resp->status != 200) {
            const std::string err = patch_resp.has_value() ? patch_resp->body : patch_resp.error();
            spdlog::error("hello-nf: heartbeat failed: {}", err);
            heartbeat_span->SetStatus(opentelemetry::trace::StatusCode::kError, "heartbeat failed");
            heartbeat_span->End();
            return 1;
        }
        spdlog::info("hello-nf: heartbeat OK (HTTP {})", patch_resp->status);
        heartbeat_span->End();
    }

    sbi_core::http2::ClientRequest del_req;
    del_req.method = "DELETE";
    del_req.url = nrf_base + "/nnrf-nfm/v1/nf-instances/" + nf_instance_id;
    del_req.headers.emplace("authorization", "Bearer " + *token);

    auto del_resp = http_client.send(del_req);
    if (!del_resp.has_value() || del_resp->status != 204) {
        const std::string err = del_resp.has_value() ? del_resp->body : del_resp.error();
        spdlog::error("hello-nf: deregistration failed: {}", err);
        return 1;
    }
    spdlog::info("hello-nf: deregistered (HTTP {})", del_resp->status);

    spdlog::info("hello-nf: lifecycle complete");
    return 0;
}
