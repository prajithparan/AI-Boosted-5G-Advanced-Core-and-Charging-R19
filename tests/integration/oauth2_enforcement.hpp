#pragma once

// ADR-0499 phase 4: the access token checks one enforcing NF must make, against a real nrf and the
// real NF process (TS 29.500 6.7.3, TS 33.501 13.4.1.2): no token -> 401 with a bare Bearer
// challenge, a token for another NF type -> 401 invalid_token (aud), a token for another service
// -> 403 insufficient_scope, and the NF's own token reaches the handler (`authorized_status`).

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace nf_test {

struct EnforcedRoute {
    std::string method;
    std::string url;
    std::string body;             // sent as application/json when not empty
    std::string scope;            // the API's service-level scope
    std::string nf_type;          // the producer's NFType (the token's aud)
    std::string other_scope;      // a scope the route does not accept
    long authorized_status = 200; // what the handler answers once authorized
};

inline sbi_core::http2::Client oauth2_test_client() {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

inline std::string oauth2_test_token(sbi_core::http2::Client& client,
                                     const std::string& scope,
                                     const std::string& target_nf_type) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body =
        "grant_type=client_credentials&nfInstanceId=" + std::string(nf_test::kTestAfInstanceId) +
        "&scope=" + scope + "&targetNfType=" + target_nf_type;
    const auto resp = nf_test::send_token_request(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return nlohmann::json::parse(resp->body).at("access_token").get<std::string>();
}

inline sbi_core::http2::ClientResponse
send_to(sbi_core::http2::Client& client, const EnforcedRoute& route, const std::string& token) {
    sbi_core::http2::ClientRequest req;
    req.method = route.method;
    req.url = route.url;
    if (!route.body.empty()) {
        req.headers.emplace("content-type", "application/json");
        req.body = route.body;
    }
    if (!token.empty()) {
        req.headers.emplace("authorization", "Bearer " + token);
    }
    auto resp = client.send(req);
    return resp.has_value() ? *resp : sbi_core::http2::ClientResponse{};
}

inline std::string www_authenticate(const sbi_core::http2::ClientResponse& resp) {
    const auto it = resp.headers.find("www-authenticate");
    return it == resp.headers.end() ? "" : it->second;
}

// Waits (up to ~10 s) for the NF to answer, then checks the four cases.
inline void expect_oauth2_enforced(const EnforcedRoute& route) {
    auto client = oauth2_test_client();
    sbi_core::http2::ClientResponse resp;
    for (int attempt = 0; attempt < 100 && resp.status == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        resp = send_to(client, route, "");
    }
    SCOPED_TRACE(route.method + " " + route.url);

    ASSERT_EQ(resp.status, 401) << "no token: " << resp.body;
    EXPECT_EQ(www_authenticate(resp).rfind("Bearer realm=\"", 0), 0U) << www_authenticate(resp);
    EXPECT_EQ(www_authenticate(resp).find("error="), std::string::npos)
        << "a request without a token gets no error attribute (TS 29.500 6.7.3)";

    // A token for another NF type: the test AF's own (aud AF). The NRF issues tokens only for an
    // NF type some registered NF is (ADR-0499 phase 2), and the test AF always is.
    const auto wrong_aud = oauth2_test_token(client, route.scope, "AF");
    ASSERT_FALSE(wrong_aud.empty());
    resp = send_to(client, route, wrong_aud);
    EXPECT_EQ(resp.status, 401) << resp.body;
    EXPECT_NE(www_authenticate(resp).find("error=\"invalid_token\""), std::string::npos)
        << www_authenticate(resp);

    const auto wrong_scope = oauth2_test_token(client, route.other_scope, route.nf_type);
    ASSERT_FALSE(wrong_scope.empty());
    resp = send_to(client, route, wrong_scope);
    EXPECT_EQ(resp.status, 403) << resp.body;
    EXPECT_NE(www_authenticate(resp).find("error=\"insufficient_scope\""), std::string::npos)
        << www_authenticate(resp);

    const auto token = oauth2_test_token(client, route.scope, route.nf_type);
    ASSERT_FALSE(token.empty());
    resp = send_to(client, route, token);
    EXPECT_EQ(resp.status, route.authorized_status) << resp.body;
}

} // namespace nf_test
