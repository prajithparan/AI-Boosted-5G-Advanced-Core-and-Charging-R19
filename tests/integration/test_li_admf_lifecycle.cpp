// The ADMF's LI lifecycle engine (ADR-0462 step 4): the six workflows of TS 103 120 Annex H.5, driven
// the way an LEA drives them (HI1 objects in, HI1 objects out), against REAL PostgreSQL and in-memory
// network elements that are li_core's real X1 server codec. The request phase (`handle`) and the
// review/action phase (`reconcile_once`) are exercised separately because the profile separates them
// (H.5.2.2): the LEA gets its acknowledgement first and the Notification later.
//
// Skips when no PostgreSQL is reachable. It owns the database while it runs (the tables are emptied
// first), so point LI_ADMF_DATABASE_URL at the test database, never at a live one.

#include <pqxx/pqxx>

#include <cstdlib>
#include <string>
#include <vector>

#include "hi1_store.hpp"
#include "li_admf_fakes.hpp"
#include "lifecycle.hpp"
#include "lipf.hpp"

#include <gtest/gtest.h>

namespace {

namespace hi1 = li_core::hi1;
namespace x1 = li_core::x1;
using li_admf::Lifecycle;
using li_admf::Workflow;
using li_admf_test::FakeTransport;

std::string database_url() {
    const char* env = std::getenv("LI_ADMF_DATABASE_URL");
    return env != nullptr ? env : "postgresql://li_admf:li_admf@127.0.0.1:5439/li_admf";
}

// ---- object builders -----------------------------------------------------------------------------

constexpr const char* kNs =
    R"(xmlns="http://uri.etsi.org/03120/common/2019/10/Core" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" )"
    R"(xmlns:common="http://uri.etsi.org/03120/common/2016/02/Common" xmlns:auth="http://uri.etsi.org/03120/common/2020/09/Authorisation" )"
    R"(xmlns:task="http://uri.etsi.org/03120/common/2020/09/Task" xmlns:doc="http://uri.etsi.org/03120/common/2020/09/Document" )"
    R"(xmlns:etsi="http://uri.etsi.org/03280/common/2017/07")";

std::string ts(long offset_seconds) {
    const std::time_t t = std::time(nullptr) + offset_seconds;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string uuid(int n) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-0000-4000-8000-%012x", static_cast<unsigned>(0xA0000000U + static_cast<unsigned>(n)),
                  static_cast<unsigned>(n));
    return buf;
}

hi1::Object parse(const std::string& xml) {
    auto o = hi1::Object::from_xml(xml);
    EXPECT_TRUE(o.has_value()) << (o ? "" : o.error()) << "\n" << xml;
    return o ? *o : hi1::Object{};
}

struct AuthOpts {
    std::string end = ts(86400);
    std::string start;     // empty: no StartTime
    std::string desired;   // AuthorisationDesiredStatus value, "" = absent
    std::string status;    // a (forbidden) AuthorisationStatus, for the negative test
    std::string generation;
};

hi1::Object authorisation(const std::string& id, const AuthOpts& o = {}) {
    std::string x = std::string("<HI1Object ") + kNs + R"( xsi:type="auth:AuthorisationObject"><ObjectIdentifier>)" + id +
                    "</ObjectIdentifier>";
    if (!o.generation.empty()) {
        x += "<Generation>" + o.generation + "</Generation>";
    }
    x += "<auth:AuthorisationReference>W-" + id.substr(0, 8) + "</auth:AuthorisationReference>";
    if (!o.status.empty()) {
        x += "<auth:AuthorisationStatus><common:Owner>ETSI</common:Owner><common:Name>AuthorisationStatus</common:Name><common:Value>" +
             o.status + "</common:Value></auth:AuthorisationStatus>";
    }
    if (!o.desired.empty()) {
        x += "<auth:AuthorisationDesiredStatus><common:Owner>ETSI</common:Owner><common:Name>AuthorisationDesiredStatus</common:Name><common:Value>" +
             o.desired + "</common:Value></auth:AuthorisationDesiredStatus>";
    }
    x += "<auth:AuthorisationTimespan>";
    if (!o.start.empty()) {
        x += "<auth:StartTime>" + o.start + "</auth:StartTime>";
    }
    x += "<auth:EndTime>" + o.end + "</auth:EndTime></auth:AuthorisationTimespan></HI1Object>";
    return parse(x);
}

// An update that names only the Authorisation's timespan end.
hi1::Object authorisation_end_update(const std::string& id, const std::string& end, bool with_start = false) {
    std::string x = std::string("<HI1Object ") + kNs + R"( xsi:type="auth:AuthorisationObject"><ObjectIdentifier>)" + id +
                    "</ObjectIdentifier><auth:AuthorisationTimespan>";
    if (with_start) {
        x += "<auth:StartTime>" + ts(-10) + "</auth:StartTime>";
    }
    x += "<auth:EndTime>" + end + "</auth:EndTime></auth:AuthorisationTimespan></HI1Object>";
    return parse(x);
}

hi1::Object authorisation_desired(const std::string& id, const std::string& desired) {
    return parse(std::string("<HI1Object ") + kNs + R"( xsi:type="auth:AuthorisationObject"><ObjectIdentifier>)" + id +
                 "</ObjectIdentifier><auth:AuthorisationDesiredStatus><common:Owner>ETSI</common:Owner><common:Name>AuthorisationDesiredStatus</common:Name><common:Value>" +
                 desired + "</common:Value></auth:AuthorisationDesiredStatus></HI1Object>");
}

struct TaskOpts {
    std::string imsi = "999700000000001";
    std::string format = "SUPIIMSI";
    std::string address = "192.0.2.10:9443";
    std::string delivery = "IRIOnly";
    std::string start;
    std::string end;
    std::string desired;
};

hi1::Object task(const std::string& id, const std::string& auth_id, const std::string& liid, const TaskOpts& o = {}) {
    std::string x = std::string("<HI1Object ") + kNs + R"( xsi:type="task:LITaskObject"><ObjectIdentifier>)" + id +
                    "</ObjectIdentifier><AssociatedObjects><AssociatedObject>" + auth_id +
                    "</AssociatedObject></AssociatedObjects><task:Reference>" + liid + "</task:Reference>";
    if (!o.desired.empty()) {
        x += "<task:DesiredStatus><common:Owner>ETSI</common:Owner><common:Name>TaskDesiredStatus</common:Name><common:Value>" +
             o.desired + "</common:Value></task:DesiredStatus>";
    }
    if (!o.start.empty() || !o.end.empty()) {
        x += "<task:Timespan>";
        if (!o.start.empty()) {
            x += "<task:StartTime>" + o.start + "</task:StartTime>";
        }
        if (!o.end.empty()) {
            x += "<task:EndTime>" + o.end + "</task:EndTime>";
        }
        x += "</task:Timespan>";
    }
    x += "<task:TargetIdentifier><task:TargetIdentifierValues><task:TargetIdentifierValue><task:FormatType><task:FormatOwner>ETSI</task:FormatOwner>"
         "<task:FormatName>" + o.format + "</task:FormatName></task:FormatType><task:Value>" + o.imsi +
         "</task:Value></task:TargetIdentifierValue></task:TargetIdentifierValues></task:TargetIdentifier>"
         "<task:DeliveryType><common:Owner>ETSI</common:Owner><common:Name>TaskDeliveryType</common:Name><common:Value>" + o.delivery +
         "</common:Value></task:DeliveryType>";
    const auto colon = o.address.rfind(':');
    x += "<task:DeliveryDetails><task:DeliveryDestination><task:DeliveryAddress><task:IPAddressPort><etsi:address><etsi:IPv4Address>" +
         o.address.substr(0, colon) + "</etsi:IPv4Address></etsi:address><etsi:port><etsi:TCPPort>" + o.address.substr(colon + 1) +
         "</etsi:TCPPort></etsi:port></task:IPAddressPort></task:DeliveryAddress></task:DeliveryDestination></task:DeliveryDetails>"
         "</HI1Object>";
    return parse(x);
}

hi1::Object task_update(const std::string& id, const std::string& inner) {
    return parse(std::string("<HI1Object ") + kNs + R"( xsi:type="task:LITaskObject"><ObjectIdentifier>)" + id + "</ObjectIdentifier>" +
                 inner + "</HI1Object>");
}

std::string task_desired_xml(const std::string& value) {
    return "<task:DesiredStatus><common:Owner>ETSI</common:Owner><common:Name>TaskDesiredStatus</common:Name><common:Value>" + value +
           "</common:Value></task:DesiredStatus>";
}

hi1::Object document(const std::string& id, const std::string& associated, const std::string& content_type = "") {
    std::string x = std::string("<HI1Object ") + kNs + R"( xsi:type="doc:DocumentObject"><ObjectIdentifier>)" + id +
                    "</ObjectIdentifier><AssociatedObjects><AssociatedObject>" + associated +
                    "</AssociatedObject></AssociatedObjects><doc:DocumentName>warrant.pdf</doc:DocumentName>";
    if (!content_type.empty()) {
        x += "<doc:DocumentBody><doc:Contents>JVBERi0=</doc:Contents><doc:ContentType>" + content_type + "</doc:ContentType></doc:DocumentBody>";
    }
    x += "</HI1Object>";
    return parse(x);
}

hi1::Action create(std::uint64_t id, hi1::Object o) {
    return {id, hi1::CreateAction{std::move(o)}};
}
hi1::Action update(std::uint64_t id, hi1::Object o) {
    return {id, hi1::UpdateAction{std::move(o)}};
}

// ---- fixture -------------------------------------------------------------------------------------

class LiAdmfLifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        try {
            pqxx::connection probe(database_url());
        } catch (const std::exception&) {
            GTEST_SKIP() << "PostgreSQL is not reachable at " << database_url();
        }
        pool_ = std::make_unique<nf_config::PgPool>(database_url(), 2);
        store_ = std::make_unique<li_admf::Hi1Store>(*pool_);
        store_->ensure_schema();
        {
            auto lease = pool_->acquire();
            pqxx::work txn(lease.conn());
            txn.exec("TRUNCATE hi1_object, lipf_task");
            txn.commit();
        }
        li_admf::LipfConfig lc;
        lc.admf_identifier = "admf-01";
        lc.poi_identifier_association = x1::IdentifierAssociationEventsGenerated::All;
        lipf_ = std::make_unique<li_admf::Lipf>(
            lc,
            std::vector<li_admf::NetworkElement>{{"amf-poi", "poi", "amf-poi-01", "u1"}, {"mdf2", "mdf2", "mdf2-01", "u2"}},
            net_);
        li_admf::LifecycleConfig cfg;
        cfg.self = {"GB", "CSP-5GC-R19"};
        cfg.retry_interval = std::chrono::seconds(0);
        lifecycle_ = std::make_unique<Lifecycle>(cfg, *store_, *lipf_);
    }

    hi1::Header header() const {
        hi1::Header h;
        h.sender = lea_;
        h.receiver = {"GB", "CSP-5GC-R19"};
        h.transaction_id = uuid(900 + ++txn_);
        h.timestamp = "2026-10-06T12:00:00.000000Z";
        h.version = {"V1.23.1", "XX", "v1.0"};
        return h;
    }

    Lifecycle::Outcome run(Workflow wf, std::vector<hi1::Action> actions) {
        return lifecycle_->handle(wf, lea_, header(), actions);
    }

    const hi1::Failure* failure_of(const Lifecycle::Outcome& o, std::uint64_t id) const {
        for (const auto& r : o.results) {
            if (r.id == id) {
                return std::get_if<hi1::Failure>(&r.outcome);
            }
        }
        return nullptr;
    }

    std::string status_of(const std::string& id) {
        const auto s = store_->get(id);
        return s ? s->status : "(absent)";
    }

    std::vector<li_admf::StoredObject> notifications() {
        li_admf::ListFilter f;
        f.object_type = "Notification";
        return store_->list(f);
    }

    // The standard warrant: one authorisation, one task, one document, served on the New
    // Authorisation endpoint and reconciled to Active.
    void serve_standard_warrant(const TaskOpts& to = {}, const AuthOpts& ao = {}) {
        auto out = run(Workflow::NewAuthorisation,
                       {create(0, authorisation(uuid(1), ao)),
                        create(1, task(uuid(2), uuid(1), "LIID-1", to)),
                        create(2, document(uuid(3), uuid(1), "application/pdf"))});
        ASSERT_FALSE(out.rejected.has_value()) << out.rejected->description;
        ASSERT_EQ(out.results.size(), 3U);
        for (const auto& r : out.results) {
            ASSERT_FALSE(std::holds_alternative<hi1::Failure>(r.outcome))
                << std::get<hi1::Failure>(r.outcome).description;
        }
        lifecycle_->reconcile_once();
    }

    std::unique_ptr<nf_config::PgPool> pool_;
    std::unique_ptr<li_admf::Hi1Store> store_;
    FakeTransport net_;
    std::unique_ptr<li_admf::Lipf> lipf_;
    std::unique_ptr<Lifecycle> lifecycle_;
    hi1::EndpointId lea_{"GB", "LEA-A"};
    mutable int txn_ = 0;
};

// ---- New Authorisation (H.5.3) ---------------------------------------------------------------------

TEST_F(LiAdmfLifecycle, NewAuthorisationIsAcknowledgedFirstThenActionedAndNotified) {
    auto out = run(Workflow::NewAuthorisation,
                   {create(0, authorisation(uuid(1))),
                    create(1, task(uuid(2), uuid(1), "LIID-1")),
                    create(2, document(uuid(3), uuid(1), "application/pdf"))});
    ASSERT_FALSE(out.rejected.has_value());
    // Request phase: the Receiver set Generation 1 and the initial statuses; nothing is on the network.
    const auto* created = std::get_if<hi1::CreateResult>(&out.results[1].outcome);
    ASSERT_NE(created, nullptr);
    ASSERT_TRUE(created->object.has_value());
    EXPECT_EQ(created->object->text("Generation"), "1");
    EXPECT_EQ(created->object->entry("Status")->value, "AwaitingApproval");
    EXPECT_TRUE(net_.amf.tasks.empty());
    EXPECT_TRUE(net_.mdf.tasks.empty());
    EXPECT_TRUE(notifications().empty());

    // Review-and-action phase.
    lifecycle_->reconcile_once();
    EXPECT_EQ(status_of(uuid(1)), "Approved");
    EXPECT_EQ(status_of(uuid(2)), "Active");
    EXPECT_EQ(status_of(uuid(3)), "Approved");
    ASSERT_EQ(net_.amf.tasks.count(uuid(2)), 1U) << "the POI must hold the task";
    ASSERT_EQ(net_.mdf.tasks.count(uuid(2)), 1U);
    EXPECT_EQ(net_.mdf.tasks[uuid(2)].mediation_details[0].liid, "LIID-1");
    EXPECT_EQ(net_.amf.tasks[uuid(2)].targets[0].value, "999700000000001");

    // The Receiver-owned changes bumped Generation, and the LEA is told, with the new statuses.
    EXPECT_GE(std::stoull(*hi1::Object::from_xml(store_->get(uuid(2))->xml)->text("Generation")), 2U);
    const auto notes = notifications();
    ASSERT_EQ(notes.size(), 1U);
    auto n = hi1::Object::from_xml(notes[0].xml);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->type(), hi1::ObjectType::Notification);
    EXPECT_NE(n->text("NotificationDetails")->find("Active"), std::string::npos);
    EXPECT_EQ(notes[0].lea, "GB/LEA-A");

    // A second pass changes nothing (idempotent), in particular sends the NEs nothing.
    const auto amf_log = net_.amf.log.size();
    lifecycle_->reconcile_once();
    EXPECT_EQ(net_.amf.log.size(), amf_log);
    EXPECT_EQ(notifications().size(), 1U);
}

TEST_F(LiAdmfLifecycle, ARequestThatMissesTheWorkflowRequirementsChangesNothing) {
    // No Document (H.5.3.3 needs at least one).
    auto out = run(Workflow::NewAuthorisation,
                   {create(0, authorisation(uuid(1))), create(1, task(uuid(2), uuid(1), "LIID-1"))});
    ASSERT_TRUE(out.rejected.has_value());
    EXPECT_EQ(out.rejected->code, 3005U);
    EXPECT_FALSE(store_->get(uuid(1)).has_value());

    // A verb the endpoint does not take.
    out = run(Workflow::NewAuthorisation, {update(0, authorisation_desired(uuid(1), "Cancelled"))});
    ASSERT_TRUE(out.rejected.has_value());
    EXPECT_EQ(out.rejected->code, 3007U);
}

TEST_F(LiAdmfLifecycle, OneBadActionRefusesTheWholeRequestAndNothingIsStored) {
    // The Sender may not set a Receiver-owned status (7.2.5).
    AuthOpts bad;
    bad.status = "Approved";
    auto out = run(Workflow::NewAuthorisation,
                   {create(0, authorisation(uuid(1), bad)),
                    create(1, task(uuid(2), uuid(1), "LIID-1")),
                    create(2, document(uuid(3), uuid(1)))});
    ASSERT_FALSE(out.rejected.has_value());
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3007U);
    // The good actions say they were not applied, rather than looking accepted (H.5.2.2.4).
    // The task links to the authorisation that was refused in the same message: Annex D 3018.
    ASSERT_NE(failure_of(out, 1), nullptr);
    EXPECT_EQ(failure_of(out, 1)->code, 3018U);
    // An action that was itself fine is told it was not applied, rather than looking accepted.
    ASSERT_NE(failure_of(out, 2), nullptr);
    EXPECT_EQ(failure_of(out, 2)->code, 3018U); // the document links to the failed authorisation too
    EXPECT_FALSE(store_->get(uuid(1)).has_value());
    EXPECT_FALSE(store_->get(uuid(2)).has_value());
    EXPECT_FALSE(store_->get(uuid(3)).has_value());
}

TEST_F(LiAdmfLifecycle, CreateRulesDuplicateGenerationLinksNotificationsAndDocuments) {
    serve_standard_warrant();
    // 3010: the identifier already exists.
    auto out = run(Workflow::TaskAddition, {create(0, task(uuid(2), uuid(1), "LIID-X")), create(1, document(uuid(7), uuid(1)))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3010U);

    // Generation must not be specified on a CREATE (7.1.3).
    AuthOpts g;
    g.generation = "5";
    out = run(Workflow::NewAuthorisation,
              {create(0, authorisation(uuid(10), g)), create(1, task(uuid(11), uuid(10), "L")), create(2, document(uuid(12), uuid(10)))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3007U);

    // 3016: a link to an object that does not exist.
    out = run(Workflow::TaskAddition, {create(0, task(uuid(20), uuid(999), "L")), create(1, document(uuid(21), uuid(999)))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3016U);

    // 3007: only the Receiver creates NotificationObjects (7.4.1) -- exercised through a Document-less
    // Task Addition so the shape check passes first.
    hi1::NotificationParams np;
    np.identifier = uuid(30);
    np.country_code = "GB";
    np.owner_identifier = "LEA-A";
    np.details = "forged";
    np.type = {"ETSI", "NotificationType", "General"};
    np.timestamp = ts(0);
    const auto forged = hi1::make_notification(np);
    ASSERT_TRUE(forged.has_value());
    out = run(Workflow::TaskAddition, {create(0, task(uuid(31), uuid(1), "L")), create(1, document(uuid(32), uuid(1))), create(2, *forged)});
    ASSERT_TRUE(out.rejected.has_value()) << "a Notification CREATE does not even meet the endpoint's shape";

    // 3007: a document whose content type is not on the accepted list (H.5.2.3.4).
    out = run(Workflow::TaskAddition,
              {create(0, task(uuid(40), uuid(1), "L")), create(1, document(uuid(41), uuid(1), "application/x-msdownload"))});
    ASSERT_NE(failure_of(out, 1), nullptr);
    EXPECT_EQ(failure_of(out, 1)->code, 3007U);
}

TEST_F(LiAdmfLifecycle, ATaskTheDeploymentCannotCarryRejectsTheWholeAuthorisation) {
    TaskOpts cc;
    cc.delivery = "IRIandCC"; // no CC-POI exists
    auto out = run(Workflow::NewAuthorisation,
                   {create(0, authorisation(uuid(1))),
                    create(1, task(uuid(2), uuid(1), "LIID-1")),
                    create(2, task(uuid(4), uuid(1), "LIID-2", cc)),
                    create(3, document(uuid(3), uuid(1)))});
    ASSERT_FALSE(out.rejected.has_value());
    lifecycle_->reconcile_once();
    // H.5.2.2.4: some changes cannot take place -> all are rejected, and nothing reaches the network.
    EXPECT_EQ(status_of(uuid(1)), "Rejected");
    EXPECT_EQ(status_of(uuid(2)), "Rejected");
    EXPECT_EQ(status_of(uuid(4)), "Rejected");
    EXPECT_EQ(status_of(uuid(3)), "Rejected");
    EXPECT_TRUE(net_.amf.tasks.empty());
    EXPECT_TRUE(net_.mdf.tasks.empty());
    const auto notes = notifications();
    ASSERT_EQ(notes.size(), 1U);
    EXPECT_NE(hi1::Object::from_xml(notes[0].xml)->text("NotificationDetails")->find("content interception"), std::string::npos);
    // The reason is on the rejected authorisation for the LEA to read.
    const auto auth = hi1::Object::from_xml(store_->get(uuid(1))->xml);
    EXPECT_TRUE(auth->text_at({"AuthorisationInvalidReason", "ErrorDescription"}).has_value());
}

TEST_F(LiAdmfLifecycle, AFutureStartLeavesTheTaskAwaitingProvisioning) {
    TaskOpts later;
    later.start = ts(3600);
    serve_standard_warrant(later);
    EXPECT_EQ(status_of(uuid(1)), "Approved");
    EXPECT_EQ(status_of(uuid(2)), "AwaitingProvisioning");
    EXPECT_TRUE(net_.amf.tasks.empty());
}

TEST_F(LiAdmfLifecycle, ANeFailureMarksTheTaskErrorAndTheNextPassRecovers) {
    net_.amf.fail_activate = x1::ErrorCode::ActivateTaskFailure;
    serve_standard_warrant();
    EXPECT_EQ(status_of(uuid(2)), "Error");
    EXPECT_TRUE(net_.mdf.tasks.empty()) << "all-or-nothing: the MDF2 half was rolled back";
    const auto err = hi1::Object::from_xml(store_->get(uuid(2))->xml);
    EXPECT_TRUE(err->text_at({"InvalidReason", "ErrorDescription"}).has_value());

    net_.amf.fail_activate.reset(); // the NE is healthy again
    lifecycle_->reconcile_once();
    EXPECT_EQ(status_of(uuid(2)), "Active");
    EXPECT_EQ(net_.amf.tasks.count(uuid(2)), 1U);
}

// ---- Authorisation Extension (H.5.4) ---------------------------------------------------------------

TEST_F(LiAdmfLifecycle, ExtensionMovesTheEndDateAndReprovisionsTheTask) {
    serve_standard_warrant();
    const std::string new_end = ts(172800);
    auto out = run(Workflow::AuthorisationExtension,
                   {update(0, authorisation_end_update(uuid(1), new_end)),
                    update(1, task_update(uuid(2), "<task:Timespan><task:EndTime>" + new_end + "</task:EndTime></task:Timespan>")),
                    create(2, document(uuid(8), uuid(1), "application/pdf"))});
    ASSERT_FALSE(out.rejected.has_value());
    for (const auto& r : out.results) {
        ASSERT_FALSE(std::holds_alternative<hi1::Failure>(r.outcome)) << std::get<hi1::Failure>(r.outcome).description;
    }
    EXPECT_EQ(hi1::view_authorisation(*hi1::Object::from_xml(store_->get(uuid(1))->xml)).end_time, new_end);
    lifecycle_->reconcile_once();
    ASSERT_EQ(net_.mdf.tasks.count(uuid(2)), 1U);
    ASSERT_TRUE(net_.mdf.tasks[uuid(2)].mediation_details[0].end_time.has_value());
    EXPECT_EQ(net_.mdf.tasks[uuid(2)].mediation_details[0].end_time->substr(0, 19), new_end.substr(0, 19));
    EXPECT_EQ(status_of(uuid(2)), "Active");
}

TEST_F(LiAdmfLifecycle, ExtensionConstraintsOfTablesH1AndH2) {
    serve_standard_warrant();
    const std::string new_end = ts(172800);
    // Only the EndTime may be populated (H.1).
    auto out = run(Workflow::AuthorisationExtension,
                   {update(0, authorisation_end_update(uuid(1), new_end, true)), create(1, document(uuid(8), uuid(1)))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3005U);
    // An "extension" that shortens is an improper value change.
    out = run(Workflow::AuthorisationExtension,
              {update(0, authorisation_end_update(uuid(1), ts(60))), create(1, document(uuid(9), uuid(1)))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3008U);
    // A task may not outlive the authorisation (H.2).
    out = run(Workflow::AuthorisationExtension,
              {update(0, authorisation_end_update(uuid(1), new_end)),
               update(1, task_update(uuid(2), "<task:Timespan><task:EndTime>" + ts(999999) + "</task:EndTime></task:Timespan>")),
               create(2, document(uuid(10), uuid(1)))});
    ASSERT_NE(failure_of(out, 1), nullptr);
    EXPECT_EQ(failure_of(out, 1)->code, 3007U);
}

// ---- Authorisation / Task Cancellation (H.5.5, H.5.7) --------------------------------------------------

TEST_F(LiAdmfLifecycle, AuthorisationCancellationCancelsEveryTaskAndTakesThemOffTheNetwork) {
    serve_standard_warrant();
    ASSERT_EQ(net_.amf.tasks.size(), 1U);
    auto out = run(Workflow::AuthorisationCancellation, {update(0, authorisation_desired(uuid(1), "Cancelled")), create(1, document(uuid(8), uuid(1)))});
    ASSERT_FALSE(out.rejected.has_value());
    lifecycle_->reconcile_once();
    EXPECT_EQ(status_of(uuid(1)), "Cancelled");
    EXPECT_EQ(status_of(uuid(2)), "Cancelled") << "H.5.5.3: the LITasks of the authorisation are cancelled too";
    EXPECT_TRUE(net_.amf.tasks.empty());
    EXPECT_TRUE(net_.mdf.tasks.empty());
    EXPECT_TRUE(net_.mdf.destinations.empty());
    // A cancelled authorisation takes no more updates (3012).
    out = run(Workflow::AuthorisationExtension, {update(0, authorisation_end_update(uuid(1), ts(999999))), create(1, document(uuid(9), uuid(1)))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3012U);
    // The desired status must be Cancelled (H.3).
    EXPECT_EQ(run(Workflow::AuthorisationCancellation, {update(0, authorisation_desired(uuid(1), "Approved"))}).results.size(), 1U);
}

TEST_F(LiAdmfLifecycle, TaskAdditionThenTaskCancellationTouchesOnlyThatTask) {
    serve_standard_warrant();
    auto out = run(Workflow::TaskAddition, {create(0, task(uuid(5), uuid(1), "LIID-5", TaskOpts{"999700000000002"})), create(1, document(uuid(6), uuid(1)))});
    ASSERT_FALSE(out.rejected.has_value());
    lifecycle_->reconcile_once();
    EXPECT_EQ(status_of(uuid(5)), "Active");
    EXPECT_EQ(net_.amf.tasks.size(), 2U);

    out = run(Workflow::TaskCancellation, {update(0, task_update(uuid(5), task_desired_xml("Cancelled")))});
    ASSERT_FALSE(out.rejected.has_value());
    lifecycle_->reconcile_once();
    EXPECT_EQ(status_of(uuid(5)), "Cancelled");
    EXPECT_EQ(status_of(uuid(2)), "Active") << "the other task under the same warrant keeps running (H.5.7.1)";
    EXPECT_EQ(net_.amf.tasks.count(uuid(2)), 1U);
    EXPECT_EQ(net_.amf.tasks.count(uuid(5)), 0U);
    EXPECT_EQ(status_of(uuid(1)), "Approved");
}

TEST_F(LiAdmfLifecycle, TaskAdditionToACancelledAuthorisationIsRefused) {
    serve_standard_warrant();
    (void)run(Workflow::AuthorisationCancellation, {update(0, authorisation_desired(uuid(1), "Cancelled"))});
    lifecycle_->reconcile_once();
    const auto out = run(Workflow::TaskAddition, {create(0, task(uuid(5), uuid(1), "L")), create(1, document(uuid(6), uuid(1)))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3017U);
}

// ---- Change of Delivery (H.5.8) ---------------------------------------------------------------------

TEST_F(LiAdmfLifecycle, ChangeOfDeliveryMovesTheDestinationOnTheMdf2) {
    serve_standard_warrant();
    ASSERT_EQ(net_.mdf.destinations.size(), 1U);
    const std::string moved =
        "<task:DeliveryDetails><task:DeliveryDestination><task:DeliveryAddress><task:IPAddressPort><etsi:address><etsi:IPv4Address>198.51.100.7"
        "</etsi:IPv4Address></etsi:address><etsi:port><etsi:TCPPort>9443</etsi:TCPPort></etsi:port></task:IPAddressPort></task:DeliveryAddress>"
        "</task:DeliveryDestination></task:DeliveryDetails>";
    const auto out = run(Workflow::ChangeOfDelivery, {update(0, task_update(uuid(2), moved))});
    ASSERT_FALSE(out.rejected.has_value());
    ASSERT_EQ(failure_of(out, 0), nullptr);
    lifecycle_->reconcile_once();
    ASSERT_EQ(net_.mdf.destinations.size(), 1U) << "the old destination is retired, the new one created";
    EXPECT_EQ(net_.mdf.destinations.begin()->second.address.value, "198.51.100.7:9443");
    EXPECT_EQ(status_of(uuid(2)), "Active");

    // An UPDATE that carries no DeliveryDetails is not a change of delivery (H.5).
    const auto bad = run(Workflow::ChangeOfDelivery, {update(0, task_update(uuid(2), task_desired_xml("Active")))});
    ASSERT_NE(failure_of(bad, 0), nullptr);
    EXPECT_EQ(failure_of(bad, 0)->code, 3005U);
}

// ---- time, ownership, updates ----------------------------------------------------------------------------

TEST_F(LiAdmfLifecycle, AnExpiredAuthorisationExpiresItsTasksAndTheyLeaveTheNetwork) {
    serve_standard_warrant();
    ASSERT_EQ(net_.amf.tasks.size(), 1U);
    // Make the authorisation's end date pass (the LEA-side UPDATE path would refuse this, so edit
    // the stored object the way the passage of time does).
    auto stored = *store_->get(uuid(1));
    auto obj = *hi1::Object::from_xml(stored.xml);
    ASSERT_TRUE(obj.merge(authorisation_end_update(uuid(1), ts(-5))).has_value());
    stored.xml = obj.xml();
    ASSERT_TRUE(store_->replace(stored, stored.generation));
    lifecycle_->reconcile_once();
    EXPECT_EQ(status_of(uuid(1)), "Expired");
    EXPECT_EQ(status_of(uuid(2)), "Expired");
    EXPECT_TRUE(net_.amf.tasks.empty());
}

TEST_F(LiAdmfLifecycle, AnLeaSeesOnlyItsOwnObjects) {
    serve_standard_warrant();
    const hi1::EndpointId other{"GB", "LEA-B"};
    hi1::Header h = header();
    h.sender = other;
    // GET and LIST at the base URL, as another LEA.
    auto got = lifecycle_->handle(Workflow::None, other, h, {{0, hi1::GetAction{uuid(1)}}});
    ASSERT_NE(failure_of(got, 0), nullptr);
    EXPECT_EQ(failure_of(got, 0)->code, 3014U);
    auto listed = lifecycle_->handle(Workflow::None, other, h, {{0, hi1::ListAction{}}});
    EXPECT_TRUE(std::get<hi1::ListResult>(listed.results[0].outcome).records.empty());
    // ...and cannot update them.
    auto upd = lifecycle_->handle(Workflow::AuthorisationCancellation, other, h,
                                  {update(0, authorisation_desired(uuid(1), "Cancelled"))});
    ASSERT_NE(failure_of(upd, 0), nullptr);
    EXPECT_EQ(failure_of(upd, 0)->code, 3011U);

    // The owner sees everything of its own, including the Notification the CSP issued.
    auto own = run(Workflow::None, {{0, hi1::GetAction{uuid(2)}}, {1, hi1::ListAction{}}});
    ASSERT_TRUE(std::holds_alternative<hi1::GetResult>(own.results[0].outcome));
    EXPECT_GE(std::get<hi1::ListResult>(own.results[1].outcome).records.size(), 4U); // auth, task, doc, notification
    hi1::ListAction only_notifications;
    only_notifications.object_type = hi1::DictionaryEntry{"ETSI", "ObjectType", "Notification"};
    own = run(Workflow::None, {{0, only_notifications}});
    EXPECT_EQ(std::get<hi1::ListResult>(own.results[0].outcome).records.size(), 1U);
}

TEST_F(LiAdmfLifecycle, UpdateRulesStatusGenerationAndMerge) {
    serve_standard_warrant();
    // The Sender may not set the Receiver-owned status (6.4.7 / 8.2.3).
    auto out = run(Workflow::TaskCancellation,
                   {update(0, task_update(uuid(2), task_desired_xml("Cancelled") + "<task:Status><common:Owner>ETSI</common:Owner>"
                                                   "<common:Name>TaskStatus</common:Name><common:Value>Active</common:Value></task:Status>"))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3006U);
    // A stale Generation is refused (7.1.3).
    out = run(Workflow::TaskCancellation, {update(0, task_update(uuid(2), "<Generation>1</Generation>" + task_desired_xml("Cancelled")))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3008U);
    // An unknown object cannot be updated.
    out = run(Workflow::TaskCancellation, {update(0, task_update(uuid(404), task_desired_xml("Cancelled")))});
    ASSERT_NE(failure_of(out, 0), nullptr);
    EXPECT_EQ(failure_of(out, 0)->code, 3011U);
    EXPECT_EQ(status_of(uuid(2)), "Active"); // none of the above changed anything
}

} // namespace
