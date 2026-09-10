// ADR-0286: the N28/Sy chain end to end -- CHF -> PCF -> SMF.
//
// This exists because of a standing user directive (2026-08-16) that N28/Sy must be real end to
// end with PCF **and SMF**, and because a status re-check on 2026-09-05 found the SMF half absent:
// PCF<->CHF worked and `test_n28_spending_limit.cpp` passed, while SMF had no `policyCounterId`
// code at all and nothing served the `pcf-notify` callback SMF had been advertising to PCF since
// ADR-0038. A green PCF<->CHF test had made the chain look finished.
//
// What is asserted here is the link that was missing: a spending-limit status change arriving at
// PCF results in a real SmPolicyNotification reaching the SMF that owns the session, and SMF
// recording it against that session. The status push is made directly to PCF's own
// `spending-limit-notify` callback -- the same request CHF makes -- so the test drives the real
// receiver rather than reaching inside PCF.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/multipart.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "spawn_guard.hpp"

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
        req.method = "POST";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
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

// The same real Prometheus scrape helper test_n28_spending_limit.cpp uses, including its own
// hard-won parsing note (a naive find() matches the `# HELP` line first). Duplicated rather than
// shared for the reason every helper in this suite is: each integration test is a standalone
// translation unit.
long scrape_metric_value(const char* host, int port, const std::string& metric_name) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct timeval tv {
        2, 0
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    const std::string req =
        "GET /metrics HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n\r\n";
    if (send(sock, req.data(), req.size(), 0) < 0) {
        close(sock);
        return -1;
    }
    std::string body;
    char buf[4096];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        body.append(buf, static_cast<std::size_t>(n));
    }
    close(sock);

    // Real Prometheus text exposition format: each metric is preceded by real `# HELP <name>
    // <description>` and `# TYPE <name> <type>` comment lines whose OWN text also contains the
    // bare metric name -- real bug found and fixed via actual test execution (not assumed): a
    // naive `body.find(metric_name)` matches the `# HELP` line first (its description text
    // happens to contain the metric name too) and then fails to parse a number out of prose,
    // silently returning -1 forever. Fixed by anchoring the search to the real VALUE line
    // specifically: "\n<metric_name> " (unlabeled counter, this project's own real shape for
    // every metric this test scrapes) or "\n<metric_name>{" (labeled) at the START of a line, not
    // anywhere in the text.
    const std::string unlabeled_needle = "\n" + metric_name + " ";
    const std::string labeled_needle = "\n" + metric_name + "{";
    auto name_pos = body.find(unlabeled_needle);
    std::size_t value_start;
    if (name_pos != std::string::npos) {
        value_start = name_pos + unlabeled_needle.size();
    } else if ((name_pos = body.find(labeled_needle)) != std::string::npos) {
        const auto brace_end = body.find('}', name_pos);
        if (brace_end == std::string::npos) {
            return -1;
        }
        value_start = brace_end + 2; // "} " before the value
    } else {
        return 0; // metric not yet emitted at all == real zero value, not an error
    }
    const auto line_end = body.find('\n', value_start);
    try {
        return std::stol(body.substr(value_start, line_end - value_start));
    } catch (const std::exception&) {
        return -1;
    }
}

} // namespace

TEST(N28SyEndToEnd, SpendingLimitStatusChangeReachesTheSmfThatOwnsTheSession) {
    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    nf_test::SpawnedProcess chf(CHF_PATH);
    ASSERT_GT(chf.pid(), 0);
    // UDR is not optional here, and finding that out is half of what this test documents: PCF only
    // opens a CHF spending-limit subscription when the subscriber's own UDR policy data says
    // `subscSpendingLimits: true` with at least one policyCounterId. Without UDR there is no
    // subscription, and the status push has nothing to attach to -- which is exactly how this test
    // failed on its third run, with "No tracked spending-limit subscription".
    nf_test::SpawnedProcess udr(UDR_PATH);
    ASSERT_GT(udr.pid(), 0);
    // ADR-0328: UPF is what makes this test about ENFORCEMENT rather than only delivery. Without
    // it the SM context has no N4 session, so SMF records the decision and correctly declines to
    // apply it -- which is the state ADR-0286 left behind and the state this test now proves is
    // gone. Spawned before SMF so the PFCP association is up when SMF establishes the session.
    nf_test::SpawnedProcess upf(UPF_PATH);
    ASSERT_GT(upf.pid(), 0);
    nf_test::SpawnedProcess pcf(PCF_PATH);
    ASSERT_GT(pcf.pid(), 0);
    nf_test::SpawnedProcess smf(SMF_PATH);
    ASSERT_GT(smf.pid(), 0);

    auto client = make_client();
    ASSERT_TRUE(wait_reachable(
        client, "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/nonexistent/retrieve", 200))
        << "smf never became reachable";
    ASSERT_TRUE(
        wait_reachable(client, "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies", 200))
        << "pcf never became reachable";

    const std::string smf_token = fetch_token(client, "nsmf-pdusession", "SMF");
    ASSERT_FALSE(smf_token.empty());

    // Seed the subscriber's real policy data with the counter this test drives. The counter id
    // matches config/pcf.json's `policy_counter_actions` entry -- the operator-owned mapping is
    // the whole point, so the test uses the configured id rather than inventing one.
    const std::string supi = "imsi-999700000000811";
    {
        const std::string udr_url =
            "https://127.0.0.1:7781/nudr-dr/v2/policy-data/ues/" + supi + "/sm-data";
        ASSERT_TRUE(wait_reachable(client, udr_url, 200)) << "udr never became reachable";
        const std::string udr_token = fetch_token(client, "nudr-dr", "UDR");
        ASSERT_FALSE(udr_token.empty());

        sbi_core::http2::ClientRequest seed;
        seed.method = "PATCH";
        seed.url = udr_url;
        seed.headers.emplace("content-type", "application/merge-patch+json");
        seed.headers.emplace("authorization", "Bearer " + udr_token);
        seed.body = json{{"smPolicySnssaiData",
                          {{"1-000001",
                            {{"snssai", {{"sst", 1}, {"sd", "000001"}}},
                             {"smPolicyDnnData",
                              {{"internet",
                                {{"dnn", "internet"},
                                 {"subscSpendingLimits", true},
                                 {"spendLimInfo",
                                  {{"quota-exhausted",
                                    {{"policyCounterId", "quota-exhausted"},
                                     {"currentStatus", "active"}}}}}}}}}}}}}}
                        .dump();
        auto seeded = client.send(seed);
        ASSERT_TRUE(seeded.has_value());
        ASSERT_EQ(seeded->status, 200) << seeded->body;
    }

    // A real SM context, which makes SMF create a real SM Policy Association with PCF -- that is
    // what gives PCF the notificationUri this whole test is about.
    //
    // ADR-0328: retried until the context has a REAL N4 session, because enforcement is the point
    // now. SMF discovers UPF through NRF on a 2s retry cadence and skips N4 establishment until
    // that association is up, while still answering 201 -- so a single unlucky-but-successful
    // creation yields a session with no `upSeid`, and enforcement correctly declines to apply
    // anything. That is exactly how this test failed once the enforcement assertion was added:
    // "no UPF Sx Association established yet, skipping N4 Session Establishment".
    //
    // Each attempt creates one more SM Policy at PCF, and PCF allocates "smpolicy-N"
    // sequentially, so the policy id is COUNTED rather than assumed to be smpolicy-1. Counting is
    // deterministic; assuming is what made an earlier version of this test depend on winning a
    // race it did not know it was in.
    std::string sm_context_ref;
    int sm_policies_created = 0;
    for (int attempt = 0; attempt < 40 && sm_context_ref.empty(); ++attempt) {
        sbi_core::multipart::Part part;
        part.content_type = "application/json";
        part.body =
            json{
                {"servingNfId", "00000000-0000-4000-8000-0000000000aa"},
                {"servingNetwork", json{{"mcc", "999"}, {"mnc", "70"}}},
                {"anType", "3GPP_ACCESS"},
                {"smContextStatusUri", "https://example.com/sm-status"},
                {"supi", supi},
                {"pduSessionId", 7 + attempt},
                {"dnn", "internet"},
                {"sNssai", json{{"sst", 1}, {"sd", "000001"}}},
            }
                .dump();
        const auto encoded = sbi_core::multipart::encode({part});

        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts";
        req.headers.emplace("content-type", encoded.content_type_header);
        req.headers.emplace("authorization", "Bearer " + smf_token);
        req.body = encoded.body;
        auto resp = client.send(req);
        if (!resp.has_value() || resp->status != 201) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        ++sm_policies_created;
        const auto loc = resp->headers.find("location");
        ASSERT_NE(loc, resp->headers.end());
        const std::string ref = loc->second.substr(loc->second.rfind('/') + 1);
        ASSERT_FALSE(ref.empty());

        // HANDOVER_REQUIRED answers 200 only once a real UPF N3 F-TEID is on record, so it is a
        // precise probe for "this context has a real N4 session" -- the same readiness check
        // test_smf_n2sminfo_dispatch.cpp uses, rather than a sleep.
        sbi_core::http2::ClientRequest probe;
        probe.method = "POST";
        probe.url = "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + ref + "/modify";
        probe.headers.emplace("content-type", "application/json");
        probe.headers.emplace("authorization", "Bearer " + smf_token);
        probe.body = json{{"n2SmInfoType", "HANDOVER_REQUIRED"}}.dump();
        if (auto probe_resp = client.send(probe);
            probe_resp.has_value() && probe_resp->status == 200) {
            sm_context_ref = ref;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    ASSERT_FALSE(sm_context_ref.empty())
        << "SMF never reached a state with a real UPF N4 session, so enforcement could not be "
           "exercised";

    // Find the SM policy PCF created for it. PCF assigns its own id, so ask PCF rather than guess.
    const std::string pcf_token = fetch_token(client, "npcf-smpolicycontrol", "PCF");
    ASSERT_FALSE(pcf_token.empty());

    // PCF allocates "smpolicy-N" sequentially (pcf/src/stores.cpp) and SMF creates exactly one
    // SM Policy per successful CreateSMContext, so the policy belonging to the context above is
    // the Nth -- where N is the number of contexts this test actually created, not 1. A wrong id
    // makes the status push 404 and the test says exactly that.
    const std::string sm_policy_id = "smpolicy-" + std::to_string(sm_policies_created);

    // The status change CHF would push. Same request shape, sent to PCF's real callback.
    {
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        // TS 29.594 delivers a SpendingLimitStatus to {notifUri}/notify -- PCF hands CHF
        // ".../spending-limit-notify" and serves ".../spending-limit-notify/notify". Using the
        // notifUri without the suffix 404s, which is how this test first failed.
        req.url = "https://127.0.0.1:7783/npcf-smpolicycontrol/v1/sm-policies/" + sm_policy_id +
                  "/spending-limit-notify/notify";
        req.headers.emplace("content-type", "application/json");
        req.body =
            json{
                {"supi", supi},
                {"statusInfos",
                 json{{"quota-exhausted",
                       json{{"policyCounterId", "quota-exhausted"},
                            {"currentStatus", "terminate"}}}}},
            }
                .dump();
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        ASSERT_EQ(resp->status, 204)
            << "PCF did not accept the spending-limit status change: " << resp->body;
    }

    // PCF pushes to SMF synchronously inside that handler, so by the time the 204 came back the
    // notification has either landed or failed. Retrieve the SM context and look for it.
    sbi_core::http2::ClientRequest retrieve;
    retrieve.method = "POST";
    retrieve.url =
        "https://127.0.0.1:7779/nsmf-pdusession/v1/sm-contexts/" + sm_context_ref + "/retrieve";
    retrieve.headers.emplace("content-type", "application/json");
    retrieve.headers.emplace("authorization", "Bearer " + smf_token);
    retrieve.body = json::object().dump();
    auto retrieved = client.send(retrieve);
    ASSERT_TRUE(retrieved.has_value());
    ASSERT_EQ(retrieved->status, 200) << retrieved->body;

    // `/retrieve` answers with a spec-shaped SmContextRetrievedData, which has nowhere to carry an
    // internal field -- and adding one would mean putting a non-spec key in a spec response. So the
    // assertion is on the counters both NFs export, which is the same reasoning
    // test_n28_spending_limit.cpp already applies to PCF's internal tracking state.
    EXPECT_EQ(scrape_metric_value("127.0.0.1", 9470, "pcf_sm_policy_updates_pushed_total"), 1)
        << "PCF did not push the operator-configured decision to the SMF that owns the session";
    EXPECT_EQ(scrape_metric_value("127.0.0.1", 9466, "smf_pcf_policy_updates_total"), 1)
        << "SMF never received the policy update -- the N28 chain stops at PCF, which is exactly "
           "the gap this test exists to prevent regressing";

    // ADR-0328: the half ADR-0286 deferred. Receiving the decision and acting on it are different
    // facts, which is why these are two counters rather than one -- `updates_total` above can be 1
    // while this is 0, and that combination is precisely the "recorded but never enforced" state
    // the directive was about.
    //
    // This counter only advances after a real PFCP Session Modification carrying the re-authorised
    // QER has been answered RequestAccepted by a real UPF process. config/pcf.json's operator-owned
    // action for this counter's "terminate" status is authSessAmbr 1 Kbps up and down, so what is
    // being proved is that hitting a CHF spending limit actually throttles the subscriber's
    // session on the user plane.
    EXPECT_EQ(scrape_metric_value("127.0.0.1", 9466, "smf_pcf_policy_enforced_total"), 1)
        << "SMF received PCF's decision but never re-authorised it onto the user plane -- the "
           "subscriber hit their spending limit and is still running at full rate, which is the "
           "exact gap ADR-0286 named and ADR-0328 closes";
}
