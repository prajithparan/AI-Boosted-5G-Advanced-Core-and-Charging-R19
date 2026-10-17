// REVIEW-5ae0ef1 SEC-03 (ADR-0499 phase 5): the AAnF takes anchor-key registration and context
// removal only from the AUSF, with a token (TS 33.535 6.1), and binds an AF's afId to the AF that
// asks (the FQDN its certificate names; the NEF relays already-verified AF_IDs, TS 33.535 6.3).
// Real nrf + aanf processes.

#include "oauth2_enforcement.hpp"
#include "spawn_guard.hpp"
#include "test_tokens.hpp"

#include <gtest/gtest.h>

namespace {

constexpr const char* kAanf = "https://127.0.0.1:7824/naanf-akma/v1";

sbi_core::http2::Client client_as(const std::string& name) {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = std::string(CERTS_DIR) + "/" + name + "/cert.pem",
        .key_path = std::string(CERTS_DIR) + "/" + name + "/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

long post(sbi_core::http2::Client& c,
          const std::string& op,
          const std::string& body,
          const std::string& token) {
    return nf_test::send_to(
               c, {.method = "POST", .url = std::string(kAanf) + op, .body = body}, token)
        .status;
}

} // namespace

TEST(AanfAuthorization, OnlyTheAusfRegistersOrRemovesAndAnAfGetsOnlyItsOwnKaf) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0) << "failed to fork nrf";
    nf_test::SpawnedProcess aanf(AANF_PATH);
    ASSERT_GT(aanf.pid(), 0) << "failed to fork aanf";

    auto ausf = client_as("ausf");
    auto af = client_as("af.example.com");
    long status = 0;
    for (int i = 0; i < 100 && status == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        status = post(ausf, "/register-anchorkey", "{}", "");
    }
    const auto token = nf_test::oauth2_test_token(af, "naanf-akma", "AANF");
    ASSERT_FALSE(token.empty());

    // Anonymous register / remove: 401 (the authorizer).
    EXPECT_EQ(status, 401);
    EXPECT_EQ(post(ausf, "/remove-context", R"({"supi":"imsi-999700000000001"})", ""), 401);
    // A token, but not the AUSF: 403.
    EXPECT_EQ(post(af, "/register-anchorkey", "{}", token), 403);
    EXPECT_EQ(post(af, "/remove-context", R"({"supi":"imsi-999700000000001"})", token), 403);
    // The AUSF passes both checks (the empty body is then the handler's 400).
    EXPECT_EQ(post(ausf, "/register-anchorkey", "{}", token), 400);

    // An AF asking for another AF's afId: 403. Its own afId passes the binding (then: no such
    // A-KID, 403 K_AKMA_NOT_PRESENT -- a different 403, told apart by the cause).
    const auto other = nf_test::send_to(
        af,
        {.method = "POST",
         .url = std::string(kAanf) + "/retrieve-applicationkey",
         .body = R"({"afId":"gpsi-af.example.com.0100BC0001","aKId":"x@akma.example"})"},
        token);
    EXPECT_EQ(other.status, 403) << other.body;
    EXPECT_EQ(other.body.find("K_AKMA_NOT_PRESENT"), std::string::npos) << other.body;
    const auto own = nf_test::send_to(
        af,
        {.method = "POST",
         .url = std::string(kAanf) + "/retrieve-applicationkey",
         .body = R"({"afId":"af.example.com.0100BC0001","aKId":"x@akma.example"})"},
        token);
    EXPECT_EQ(own.status, 403) << own.body;
    EXPECT_NE(own.body.find("K_AKMA_NOT_PRESENT"), std::string::npos) << own.body;
}
