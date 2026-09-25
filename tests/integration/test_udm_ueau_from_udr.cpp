// ADR-0383: the UDM generates authentication vectors from data it reads from the UDR over Nudr
// (QueryAuthSubsData) and advances the SQN there (ModifyAuthenticationSubscription, RFC 6902
// test+replace compare-and-swap). Real nrf + udr + udm processes, real PostgreSQL, TLS 1.3 + mTLS.
//
// What this closes: ADR-0382's disclosed gap -- a subscriber onboarded through bss/provisioning's
// UdrAdapter (via the UDR's OAM provisioning API) was "provisioned" but could not authenticate,
// because the UDM only knew its own two hardcoded subscribers. Proven here by recomputing the
// vector UE-side from the ORDER's K/OPc: MAC-A, the SQN hidden in AUTN, XRES* and KAUSF must all
// match what the UDM returned.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "TS29503_Nudm_UEAU_grp.hpp"
#include "aka_crypto/hex.hpp"
#include "aka_crypto/kdf.hpp"
#include "aka_crypto/milenage.hpp"
#include "spawn_guard.hpp"
#include "udr_adapter.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

// Test-only SIM, generated for this file (not the TS 35.207 set the UDR seeds for 000..001/002).
constexpr const char* kK = "5a8d38864820197c3394b92613b20b91";
constexpr const char* kOpc = "633f5e3a1bbbc7c4f53d8a4a2d6d2b9e";
constexpr const char* kSupi = "imsi-999700000009921";
constexpr const char* kStartSqn = "000000000020";
constexpr const char* kSnn = "5G:mnc070.mcc999.3gppnetwork.org";

sbi_core::http2::TlsConfig tls_as(const std::string& identity) {
    return sbi_core::http2::TlsConfig{
        .cert_path = std::string(CERTS_DIR) + "/" + identity + "/cert.pem",
        .key_path = std::string(CERTS_DIR) + "/" + identity + "/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
}

// Onboards kSupi through the same adapter bss/provisioning uses; the resend-safe PUT resets the
// SQN to kStartSqn on every run, so the test is repeatable against a persistent database.
void onboard_test_subscriber() {
    provisioning::UdrAdapter adapter(
        provisioning::parse_udr_adapter_config(json{
            {"udr_base_url", "https://127.0.0.1:7781"},
            {"serving_plmn_id", "99970"},
            {"authentication",
             {{"method", "5G_AKA"}, {"management_field", "8000"}, {"initial_sqn", "000000000000"}}},
            {"network_profiles",
             {{"default",
               {{"snssais", json::array({{{"sst", 1}, {"sd", "000001"}}})},
                {"dnn", "internet"},
                {"ue_ambr", {{"uplink", "1 Gbps"}, {"downlink", "1 Gbps"}}}}}}},
        }),
        tls_as("provisioning"));
    auto outcome = adapter.provision(provisioning::SubscriberSpec{
        kSupi, "9997000921", "po-test", provisioning::SimCredentials{kK, kOpc, kStartSqn}});
    ASSERT_TRUE(outcome.has_value()) << outcome.error();
}

std::string
fetch_token(sbi_core::http2::Client& client, const std::string& scope, const std::string& target) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=" + scope +
               "&targetNfType=" + target;
    auto resp = client.send(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
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

sbi_core::http2::ClientRequest generate_auth_data(const std::string& supi,
                                                  const std::string& token) {
    sbi_gen::AuthenticationInfoRequest body{};
    body.servingNetworkName = kSnn;
    body.ausfInstanceId = "00000000-0000-4000-8000-000000000ddd";
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url =
        "https://127.0.0.1:7780/nudm-ueau/v1/" + supi + "/security-information/generate-auth-data";
    req.headers.emplace("content-type", "application/json");
    req.headers.emplace("authorization", "Bearer " + token);
    req.body = json(body).dump();
    return req;
}

// UE/USIM side: recover the SQN the network put in AUTN (SQN xor AK, TS 33.102 6.3.3).
aka_crypto::Sqn sqn_from_autn(const aka_crypto::Key128& k,
                              const aka_crypto::Key128& opc,
                              const aka_crypto::Key128& rand,
                              const std::array<uint8_t, 16>& autn) {
    const auto out = aka_crypto::f2345(opc, k, rand);
    aka_crypto::Sqn sqn{};
    for (size_t i = 0; i < sqn.size(); ++i) {
        sqn[i] = static_cast<uint8_t>(autn[i] ^ out.ak[i]);
    }
    return sqn;
}

struct Stack {
    nf_test::SpawnedProcess nrf;
    nf_test::SpawnedProcess udr;
    nf_test::SpawnedProcess udm;
};

Stack spawn_stack() {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udr(UDR_PATH);
    nf_test::wait_tcp_listening(7781);
    nf_test::SpawnedProcess udm(UDM_PATH);
    return Stack{std::move(nrf), std::move(udr), std::move(udm)};
}

} // namespace

TEST(UdmUeauFromUdrIntegration, OnboardedSubscriberGetsAVectorTheUeCanVerify) {
    auto stack = spawn_stack();
    sbi_core::http2::Client client(tls_as("hello-nf"));
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7780/", 100)) << "udm never reachable";
    onboard_test_subscriber();

    const std::string token = fetch_token(client, "nudm-ueau", "UDM");
    ASSERT_FALSE(token.empty());
    auto resp = client.send(generate_auth_data(kSupi, token));
    ASSERT_TRUE(resp.has_value());
    ASSERT_EQ(resp->status, 200) << resp->body;
    const auto result = json::parse(resp->body).get<sbi_gen::AuthenticationInfoResult>();
    ASSERT_TRUE(result.authenticationVector.has_value());
    const auto av = result.authenticationVector->get<sbi_gen::Av5GHeAka>();

    // Recompute UE-side from the order's K/OPc -- nothing taken from the UDM except RAND/AUTN.
    const auto k = *aka_crypto::from_hex<16>(kK);
    const auto opc = *aka_crypto::from_hex<16>(kOpc);
    const auto rand = *aka_crypto::from_hex<16>(av.rand);
    const auto autn = *aka_crypto::from_hex<16>(av.autn);
    const auto sqn = sqn_from_autn(k, opc, rand, autn);
    EXPECT_EQ(aka_crypto::to_hex(sqn), kStartSqn) << "first vector must use the provisioned SQN";
    const aka_crypto::Amf amf{autn[6], autn[7]};
    EXPECT_EQ(aka_crypto::to_hex(amf), "8000") << "AMF field from provisioning config";
    const auto mac_a = aka_crypto::f1(opc, k, rand, sqn, amf);
    EXPECT_TRUE(std::equal(mac_a.begin(), mac_a.end(), autn.begin() + 8)) << "MAC-A mismatch";

    const auto out = aka_crypto::f2345(opc, k, rand);
    const auto sqn_xor_ak = aka_crypto::sqn_xor_ak(sqn, out.ak);
    EXPECT_EQ(av.xresStar,
              aka_crypto::to_hex(aka_crypto::derive_res_star(out.ck, out.ik, kSnn, rand, out.res)));
    EXPECT_EQ(av.kausf,
              aka_crypto::to_hex(aka_crypto::derive_kausf(out.ck, out.ik, kSnn, sqn_xor_ak)));

    // The SQN write-back landed in the UDR (read through Nudr, not the table).
    sbi_core::http2::ClientRequest q;
    q.method = "GET";
    q.url = std::string("https://127.0.0.1:7781/nudr-dr/v2/subscription-data/") + kSupi +
            "/authentication-data/authentication-subscription";
    q.headers.emplace("authorization", "Bearer " + fetch_token(client, "nudr-dr", "UDR"));
    auto stored = client.send(q);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->status, 200);
    EXPECT_EQ(json::parse(stored->body)["sequenceNumber"]["sqn"], "000000000021");
}

TEST(UdmUeauFromUdrIntegration, ConcurrentVectorsNeverShareAnSqn) {
    auto stack = spawn_stack();
    sbi_core::http2::Client client(tls_as("hello-nf"));
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7780/", 100)) << "udm never reachable";
    onboard_test_subscriber();
    const std::string token = fetch_token(client, "nudm-ueau", "UDM");
    ASSERT_FALSE(token.empty());

    constexpr int kParallel = 8;
    std::vector<std::string> autns(kParallel);
    std::vector<std::string> rands(kParallel);
    std::vector<int> statuses(kParallel, 0);
    std::vector<std::thread> threads;
    for (int i = 0; i < kParallel; ++i) {
        threads.emplace_back([&, i] {
            sbi_core::http2::Client own(tls_as("hello-nf"));
            auto r = own.send(generate_auth_data(kSupi, token));
            if (!r.has_value()) {
                return;
            }
            statuses[i] = r->status;
            if (r->status == 200) {
                const auto av = json::parse(r->body)
                                    .get<sbi_gen::AuthenticationInfoResult>()
                                    .authenticationVector->get<sbi_gen::Av5GHeAka>();
                autns[i] = av.autn;
                rands[i] = av.rand;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    const auto k = *aka_crypto::from_hex<16>(kK);
    const auto opc = *aka_crypto::from_hex<16>(kOpc);
    std::set<std::string> sqns;
    for (int i = 0; i < kParallel; ++i) {
        ASSERT_EQ(statuses[i], 200) << "request " << i;
        sqns.insert(aka_crypto::to_hex(sqn_from_autn(
            k, opc, *aka_crypto::from_hex<16>(rands[i]), *aka_crypto::from_hex<16>(autns[i]))));
    }
    EXPECT_EQ(sqns.size(), static_cast<size_t>(kParallel)) << "two vectors shared an SQN";
    // Exactly the 8 consecutive values from the provisioned start, none skipped or reused.
    EXPECT_EQ(*sqns.begin(), kStartSqn);
    EXPECT_EQ(*sqns.rbegin(), "000000000027");
}

TEST(UdmUeauFromUdrIntegration, UnknownSubscriberIs404AndUdrDownIs503) {
    {
        auto stack = spawn_stack();
        sbi_core::http2::Client client(tls_as("hello-nf"));
        ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7780/", 100));
        const std::string token = fetch_token(client, "nudm-ueau", "UDM");
        auto r = client.send(generate_auth_data("imsi-999709999999999", token));
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->status, 404) << r->body;
    }
    // No UDR at all: the UDM must say "unavailable", not "no such subscriber".
    nf_test::SpawnedProcess nrf(NRF_PATH);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nf_test::SpawnedProcess udm(UDM_PATH);
    sbi_core::http2::Client client(tls_as("hello-nf"));
    ASSERT_TRUE(wait_reachable(client, "https://127.0.0.1:7780/", 100));
    const std::string token = fetch_token(client, "nudm-ueau", "UDM");
    auto r = client.send(generate_auth_data("imsi-999700000000001", token));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 503) << r->body;
}
