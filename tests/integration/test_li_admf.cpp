// The ADMF's LI_HI1 receiver (ADR-0462 step 3), at three levels:
//   * Hi1Service  -- message-level behaviour, no process, no database (TS 103 120 9.2.2, 6.2.3,
//                    6.2.5, 6.4.4, GETCSPCONFIG 6.4.11);
//   * Hi1Store    -- the PostgreSQL warrant store (skips when no database is reachable);
//   * li-admf     -- the real process over real HTTPS + mTLS, with the audit trail read back from
//                    the database (also skips without a database).
// Requests are built with li_core::hi1 (the codec the LEA side would use) and every answer is
// parsed back with it, so the receiver is judged against the same schema-validated codec.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "hi1_service.hpp"
#include "hi1_store.hpp"
#include "li_core/hi1.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
namespace hi1 = li_core::hi1;
using li_admf::Hi1Config;
using li_admf::Hi1Service;

constexpr const char* kPeerCn = "lea-sim";
constexpr const char* kSupportedVersion = "V1.23.1";

std::string database_url() {
    const char* env = std::getenv("LI_ADMF_DATABASE_URL");
    return env != nullptr ? env : "postgresql://li_admf:li_admf@127.0.0.1:5439/li_admf";
}

bool database_reachable() {
    try {
        pqxx::connection c(database_url());
        return c.is_open();
    } catch (const std::exception&) {
        return false;
    }
}

Hi1Config config() {
    Hi1Config c;
    c.self = {"GB", "CSP-5GC-R19"};
    c.national_profile_owner = "XX";
    c.national_profile_version = "v1.0";
    c.supported_etsi_versions = {kSupportedVersion};
    c.public_base_url = "https://csp.example:7808";
    c.leas = {{kPeerCn, {"GB", "LEA-SIM-01"}}};
    return c;
}

hi1::Header request_header(const std::string& txn = "c02358b2-76cf-4ba4-a8eb-f6436ccaea2e") {
    hi1::Header h;
    h.sender = {"GB", "LEA-SIM-01"};
    h.receiver = {"GB", "CSP-5GC-R19"};
    h.transaction_id = txn;
    h.timestamp = "2026-10-06T12:00:00.000000Z";
    h.version = {kSupportedVersion, "XX", "v1.0"};
    return h;
}

std::string build(const hi1::Request& request) {
    const auto xml = hi1::serialise_request(request);
    return xml.has_value() ? *xml : std::string("BUILD FAILED: ") + xml.error();
}

hi1::Request config_request(std::uint64_t id = 0) {
    hi1::Request r;
    r.header = request_header();
    r.actions = {{id, hi1::GetCspConfigAction{}}};
    return r;
}

// The answer, parsed with the same schema-validated codec.
hi1::Response answer_of(const li_admf::Hi1Reply& reply) {
    EXPECT_EQ(reply.http_status, 200);
    auto parsed = hi1::parse_response(reply.body);
    EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().detail) << "\n" << reply.body;
    return parsed.has_value() ? *parsed : hi1::Response{};
}

const hi1::Failure& top_failure(const hi1::Response& r) {
    static const hi1::Failure none{0, "not a top-level failure"};
    const auto* f = std::get_if<hi1::Failure>(&r.payload);
    return f != nullptr ? *f : none;
}

TEST(LiAdmfService, GetCspConfigPublishesTheSixLiWorkflowEndpoints) {
    const Hi1Service service(config());
    const auto reply = service.handle(kPeerCn, "/", "text/xml", build(config_request()));
    EXPECT_EQ(reply.outcome, "ok");
    const auto response = answer_of(reply);

    // 6.2.5: same transaction identifier; sender and receiver swapped.
    EXPECT_EQ(response.header.transaction_id, "c02358b2-76cf-4ba4-a8eb-f6436ccaea2e");
    EXPECT_EQ(response.header.sender, (hi1::EndpointId{"GB", "CSP-5GC-R19"}));
    EXPECT_EQ(response.header.receiver, (hi1::EndpointId{"GB", "LEA-SIM-01"}));
    EXPECT_EQ(response.header.version.etsi_version, kSupportedVersion);

    const auto& results = std::get<std::vector<hi1::ActionResult>>(response.payload);
    ASSERT_EQ(results.size(), 1U);
    const auto* cfg = std::get_if<hi1::ConfigResult>(&results[0].outcome);
    ASSERT_NE(cfg, nullptr);
    ASSERT_EQ(cfg->config.li_endpoints.size(), 6U);
    EXPECT_EQ(cfg->config.li_endpoints[0].endpoint.value, "NewAuthorisation");
    EXPECT_EQ(cfg->config.li_endpoints[0].url, "https://csp.example:7808/li/authorisation/new");
    EXPECT_EQ(cfg->config.li_endpoints[5].endpoint.value, "ChangeOfDelivery");
    EXPECT_EQ(cfg->config.li_endpoints[5].url, "https://csp.example:7808/li/task/change-delivery");
    // Only formats ETSI itself defines (Annex C) are advertised.
    ASSERT_EQ(cfg->config.targeting.size(), 4U);
    EXPECT_EQ(cfg->config.targeting[0].format_name, "SUPIIMSI");
    EXPECT_EQ(cfg->config.targeting[0].format_owner, "ETSI");
}

TEST(LiAdmfService, AnUnboundClientCertificateNeverReachesTheMessageLayer) {
    const Hi1Service service(config());
    const auto reply = service.handle("some-other-nf", "/", "text/xml", build(config_request()));
    EXPECT_EQ(reply.http_status, 403);
    EXPECT_TRUE(reply.body.empty());
}

TEST(LiAdmfService, OnlyTheXmlEncodingIsSupported) {
    const Hi1Service service(config());
    const auto reply = service.handle(kPeerCn, "/", "application/json", "{}");
    const auto response = answer_of(reply);
    EXPECT_EQ(top_failure(response).code, 3019U);
    EXPECT_FALSE(response.header.transaction_id.empty()); // a fresh one: the request was unreadable
}

TEST(LiAdmfService, AnUnparseableMessageIsATopLevelValidationError) {
    const Hi1Service service(config());
    const auto reply = service.handle(kPeerCn, "/", "text/xml", "<not-hi1/>");
    EXPECT_EQ(top_failure(answer_of(reply)).code, 3020U);

    // A schema-invalid message still gets a reply addressed to its sender, with ITS transaction id.
    std::string xml = build(config_request());
    const auto pos = xml.find("<ActionIdentifier>");
    ASSERT_NE(pos, std::string::npos);
    xml.erase(pos, xml.find("</ActionIdentifier>") + 19 - pos);
    const auto invalid = answer_of(service.handle(kPeerCn, "/", "text/xml", xml));
    EXPECT_EQ(top_failure(invalid).code, 3020U);
    EXPECT_EQ(invalid.header.transaction_id, "c02358b2-76cf-4ba4-a8eb-f6436ccaea2e");
    EXPECT_EQ(invalid.header.receiver, (hi1::EndpointId{"GB", "LEA-SIM-01"}));
}

TEST(LiAdmfService, AnUnsupportedVersionListsTheSupportedOnes) {
    const Hi1Service service(config());
    hi1::Request r = config_request();
    r.header.version.etsi_version = "V1.2.1";
    const auto failure = top_failure(answer_of(service.handle(kPeerCn, "/", "text/xml", build(r))));
    EXPECT_EQ(failure.code, 3021U);
    EXPECT_NE(failure.description.find("V1.23.1"), std::string::npos) << failure.description;
}

TEST(LiAdmfService, TheEndpointIdentitiesMustMatch) {
    const Hi1Service service(config());
    hi1::Request wrong_receiver = config_request();
    wrong_receiver.header.receiver = {"GB", "SOMEONE-ELSE"};
    EXPECT_EQ(top_failure(answer_of(service.handle(kPeerCn, "/", "text/xml", build(wrong_receiver)))).code, 3007U);

    // The mTLS peer may only speak for the EndpointID it is onboarded as.
    hi1::Request wrong_sender = config_request();
    wrong_sender.header.sender = {"GB", "LEA-IMPERSONATOR"};
    const auto reply = service.handle(kPeerCn, "/", "text/xml", build(wrong_sender));
    EXPECT_EQ(top_failure(answer_of(reply)).code, 3007U);
    EXPECT_EQ(reply.outcome, "rejected");
}

TEST(LiAdmfService, ActionIdentifiersMustStartAtZeroAndCountUp) {
    const Hi1Service service(config());
    hi1::Request skipped = config_request(1); // starts at 1
    EXPECT_EQ(top_failure(answer_of(service.handle(kPeerCn, "/", "text/xml", build(skipped)))).code, 3007U);

    hi1::Request duplicate;
    duplicate.header = request_header();
    duplicate.actions = {{0, hi1::GetCspConfigAction{}}, {0, hi1::GetCspConfigAction{}}};
    EXPECT_EQ(top_failure(answer_of(service.handle(kPeerCn, "/", "text/xml", build(duplicate)))).code, 3002U);

    hi1::Request in_order;
    in_order.header = request_header();
    in_order.actions = {{0, hi1::GetCspConfigAction{}}, {1, hi1::GetCspConfigAction{}}};
    const auto ok = answer_of(service.handle(kPeerCn, "/", "text/xml", build(in_order)));
    EXPECT_EQ(std::get<std::vector<hi1::ActionResult>>(ok.payload).size(), 2U);
}

TEST(LiAdmfService, ActionsNotYetImplementedAreRefusedExplicitlyNeverAccepted) {
    // Until step 4 lands the six LI workflows, a CREATE must not be silently "accepted": a warrant
    // that is acknowledged but not acted on is the worst failure an LI system can have.
    const Hi1Service service(config());
    hi1::Request r;
    r.header = request_header();
    r.actions = {{0, hi1::GetAction{"7dbbc880-8750-4d3c-abe7-ea4a17646045"}}};
    const auto response = answer_of(service.handle(kPeerCn, "/", "text/xml", build(r)));
    const auto& results = std::get<std::vector<hi1::ActionResult>>(response.payload);
    ASSERT_EQ(results.size(), 1U);
    const auto* failure = std::get_if<hi1::Failure>(&results[0].outcome);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->code, 3001U);
}

// ---- store (needs PostgreSQL) -------------------------------------------------------------------

class LiAdmfStore : public ::testing::Test {
protected:
    void SetUp() override {
        if (!database_reachable()) {
            GTEST_SKIP() << "PostgreSQL is not reachable at " << database_url();
        }
        pool_ = std::make_unique<nf_config::PgPool>(database_url(), 2);
        store_ = std::make_unique<li_admf::Hi1Store>(*pool_);
        store_->ensure_schema();
        suffix_ = li_admf::new_uuid().substr(0, 8);
    }
    li_admf::StoredObject object(const std::string& type, const std::string& status = "") const {
        li_admf::StoredObject o;
        o.object_id = li_admf::new_uuid();
        o.object_type = type;
        o.owner_identifier = "ACTOR-" + suffix_;
        o.country_code = "GB";
        o.generation = 0;
        o.status = status;
        o.xml = "<HI1Object/>";
        return o;
    }
    std::unique_ptr<nf_config::PgPool> pool_;
    std::unique_ptr<li_admf::Hi1Store> store_;
    std::string suffix_;
};

TEST_F(LiAdmfStore, InsertGetAndDuplicateIdentifier) {
    auto o = object("Authorisation", "Approved");
    EXPECT_TRUE(store_->insert(o));
    EXPECT_FALSE(store_->insert(o)); // 3010 at the HI1 layer
    const auto back = store_->get(o.object_id);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->object_type, "Authorisation");
    EXPECT_EQ(back->status, "Approved");
    EXPECT_EQ(back->xml, "<HI1Object/>");
    EXPECT_EQ(back->last_changed.size(), 20U); // YYYY-MM-DDTHH:MM:SSZ
    EXPECT_FALSE(store_->get("no-such-object").has_value());
}

TEST_F(LiAdmfStore, ReplaceIsOptimisticOnGeneration) {
    auto o = object("LITask", "AwaitingProvisioning");
    ASSERT_TRUE(store_->insert(o));
    auto next = o;
    next.generation = 1;
    next.status = "Active";
    next.xml = "<HI1Object>v1</HI1Object>";
    EXPECT_TRUE(store_->replace(next, 0));
    // A second writer that still believes generation 0 loses, and changes nothing.
    auto stale = o;
    stale.generation = 1;
    stale.status = "Cancelled";
    EXPECT_FALSE(store_->replace(stale, 0));
    const auto back = store_->get(o.object_id);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->generation, 1U);
    EXPECT_EQ(back->status, "Active");
    EXPECT_EQ(back->xml, "<HI1Object>v1</HI1Object>");
    EXPECT_FALSE(store_->replace(next, 7)); // wrong expected generation
}

TEST_F(LiAdmfStore, ListFiltersAreBoundParametersAndCompose) {
    auto auth = object("Authorisation", "Approved");
    auto task_a = object("LITask", "Active");
    task_a.authorisation_id = auth.object_id;
    auto task_b = object("LITask", "Cancelled");
    task_b.authorisation_id = auth.object_id;
    ASSERT_TRUE(store_->insert(auth));
    ASSERT_TRUE(store_->insert(task_a));
    ASSERT_TRUE(store_->insert(task_b));

    li_admf::ListFilter by_auth;
    by_auth.authorisation_id = auth.object_id;
    EXPECT_EQ(store_->list(by_auth).size(), 2U);
    by_auth.status = "Active";
    const auto active = store_->list(by_auth);
    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active[0].object_id, task_a.object_id);
    by_auth.status.clear();
    by_auth.maximum = 1;
    EXPECT_EQ(store_->list(by_auth).size(), 1U);
    // A value that looks like SQL is just a value.
    li_admf::ListFilter hostile;
    hostile.status = "x' OR '1'='1";
    EXPECT_TRUE(store_->list(hostile).empty());
}

TEST_F(LiAdmfStore, AuditRowsAreAppendedAndCounted) {
    const auto before = store_->audit_count();
    store_->audit({kPeerCn, "/", "c02358b2-76cf-4ba4-a8eb-f6436ccaea2e", "GB/LEA-SIM-01", 1, "ok", ""});
    store_->audit({kPeerCn, "/", "", "", 0, "rejected", "3020: not schema-valid"});
    EXPECT_EQ(store_->audit_count(), before + 2);
}

// ---- the real process ---------------------------------------------------------------------------

constexpr std::uint16_t kHi1Port = 19808;
constexpr const char* kUrl = "https://127.0.0.1:19808/";

std::string write_process_config() {
    std::ifstream in(LI_ADMF_CONFIG_TEMPLATE);
    nlohmann::json cfg = nlohmann::json::parse(in);
    cfg["hi1_port"] = kHi1Port;
    cfg["metrics_bind_address"] = "127.0.0.1:19492";
    cfg["public_base_url"] = "https://127.0.0.1:19808";
    // The test's mTLS client is the lab's hello-nf certificate, bound to the LEA endpoint below.
    cfg["lea_bindings"] = nlohmann::json::array(
        {{{"peer_cert_cn", "hello-nf"}, {"country_code", "GB"}, {"unique_identifier", "LEA-SIM-01"}}});
    const std::string path = std::string(::testing::TempDir()) + "li_admf_test.json";
    std::ofstream(path) << cfg.dump(2);
    return path;
}

sbi_core::http2::Client client_with(const char* nf) {
    sbi_core::http2::TlsConfig tls{
        .cert_path = std::string(CERTS_DIR "/") + nf + "/cert.pem",
        .key_path = std::string(CERTS_DIR "/") + nf + "/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

std::optional<sbi_core::http2::ClientResponse> post(sbi_core::http2::Client& c,
                                                    const std::string& url,
                                                    const std::string& body,
                                                    const char* content_type = "text/xml") {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = url;
    req.headers.emplace("content-type", content_type);
    req.body = body;
    auto r = c.send(req);
    return r.has_value() ? std::optional(*r) : std::nullopt;
}

TEST(LiAdmfProcess, AnLeaGetsTheCspConfigOverMtlsAndEveryExchangeIsAudited) {
    if (!database_reachable()) {
        GTEST_SKIP() << "PostgreSQL is not reachable at " << database_url();
    }
    nf_config::PgPool pool(database_url(), 1);
    li_admf::Hi1Store store(pool);
    store.ensure_schema();
    const auto audited_before = store.audit_count();

    const auto config_file = write_process_config();
    ::setenv("LI-ADMF_CONFIG_FILE", config_file.c_str(), 1);
    nf_test::SpawnedProcess admf{LI_ADMF_PATH};
    ::unsetenv("LI-ADMF_CONFIG_FILE");
    ASSERT_GT(admf.pid(), 0);

    auto lea = client_with("hello-nf");
    std::optional<sbi_core::http2::ClientResponse> ok;
    for (int attempt = 0; attempt < 100 && !ok.has_value(); ++attempt) {
        ok = post(lea, kUrl, build(config_request()));
        if (!ok.has_value()) {
            std::this_thread::sleep_for(100ms);
        }
    }
    ASSERT_TRUE(ok.has_value()) << "li-admf never answered on its HI1 port";
    EXPECT_EQ(ok->status, 200);
    const auto parsed = hi1::parse_response(ok->body);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail << "\n" << ok->body;
    const auto& results = std::get<std::vector<hi1::ActionResult>>(parsed->payload);
    const auto* cfg = std::get_if<hi1::ConfigResult>(&results.at(0).outcome);
    ASSERT_NE(cfg, nullptr);
    ASSERT_EQ(cfg->config.li_endpoints.size(), 6U);
    EXPECT_EQ(cfg->config.li_endpoints[0].url, "https://127.0.0.1:19808/li/authorisation/new");

    // A workflow path answers too (same receiver, same message layer).
    const auto wf = post(lea, "https://127.0.0.1:19808/li/authorisation/new", build(config_request()));
    ASSERT_TRUE(wf.has_value());
    EXPECT_EQ(wf->status, 200);

    // A garbage body is a top-level 3020, over HTTP 200 (9.3.3: status codes never carry HI1 errors).
    const auto bad = post(lea, kUrl, "<nope/>");
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(bad->status, 200);
    const auto bad_parsed = hi1::parse_response(bad->body);
    ASSERT_TRUE(bad_parsed.has_value());
    EXPECT_EQ(std::get<hi1::Failure>(bad_parsed->payload).code, 3020U);

    // A client whose certificate is valid but not onboarded is refused at the transport layer.
    auto stranger = client_with("amf");
    const auto refused = post(stranger, kUrl, build(config_request()));
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->status, 403);

    // Four exchanges, four audit rows -- including the refusal.
    EXPECT_EQ(store.audit_count(), audited_before + 4);
}

std::string x1_report(const std::string& type, const std::string& ne_identifier, const std::string& body) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<X1Request xmlns="http://uri.etsi.org/03221/X1/2017/10" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <x1RequestMessage xsi:type=")" + type + R"(">
    <admfIdentifier>admf-01</admfIdentifier>
    <neIdentifier>)" + ne_identifier + R"(</neIdentifier>
    <messageTimestamp>2026-10-06T00:00:00.000000Z</messageTimestamp>
    <version>v1.23.1</version>
    <x1TransactionId>2b1e4f6a-0000-4000-8000-0000000000b1</x1TransactionId>)" + body + R"(
  </x1RequestMessage>
</X1Request>)";
}

TEST(LiAdmfProcess, TheNesReportToTheAdmfOverX1AndOnlyTheirOwnCertificateIsAccepted) {
    if (!database_reachable()) {
        GTEST_SKIP() << "PostgreSQL is not reachable at " << database_url();
    }
    const auto config_file = write_process_config();
    ::setenv("LI-ADMF_CONFIG_FILE", config_file.c_str(), 1);
    nf_test::SpawnedProcess admf{LI_ADMF_PATH};
    ::unsetenv("LI-ADMF_CONFIG_FILE");
    ASSERT_GT(admf.pid(), 0);

    // The AMF POI's own identity: certificate CN "amf", neIdentifier "amf-poi-01" (config/li-admf.json).
    auto amf = client_with("amf");
    const std::string url = "https://127.0.0.1:19808/X1/ADMF";
    const std::string ne_issue = R"(<typeOfNeIssueMessage>FaultReport</typeOfNeIssueMessage><description>no X1 request within TIME_P2</description>)";
    std::optional<sbi_core::http2::ClientResponse> ok;
    for (int attempt = 0; attempt < 100 && !ok.has_value(); ++attempt) {
        ok = post(amf, url, x1_report("ReportNEIssueRequest", "amf-poi-01", ne_issue), "application/xml");
        if (!ok.has_value()) {
            std::this_thread::sleep_for(100ms);
        }
    }
    ASSERT_TRUE(ok.has_value()) << "li-admf never answered on /X1/ADMF";
    EXPECT_EQ(ok->status, 200);
    EXPECT_NE(ok->body.find("ReportNEIssueResponse"), std::string::npos) << ok->body;

    // A task this ADMF never provisioned: X1 2020.
    const auto stray = post(amf, url,
                            x1_report("ReportTaskIssueRequest", "amf-poi-01",
                                      "<xId>11111111-1111-4111-8111-111111111111</xId><taskReportType>TerminatingFault</taskReportType>"),
                            "application/xml");
    ASSERT_TRUE(stray.has_value());
    EXPECT_NE(stray->body.find("2020"), std::string::npos) << stray->body;

    // The certificate is the AMF's but the request claims to be another NE: X1 1060.
    const auto wrong_ne = post(amf, url, x1_report("ReportNEIssueRequest", "mdf2-01", ne_issue), "application/xml");
    ASSERT_TRUE(wrong_ne.has_value());
    EXPECT_NE(wrong_ne->body.find("1060"), std::string::npos) << wrong_ne->body;

    // A certificate that is not a configured network element is refused outright.
    auto stranger = client_with("hello-nf");
    const auto refused = post(stranger, url, x1_report("ReportNEIssueRequest", "amf-poi-01", ne_issue), "application/xml");
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->status, 403);
}

} // namespace
