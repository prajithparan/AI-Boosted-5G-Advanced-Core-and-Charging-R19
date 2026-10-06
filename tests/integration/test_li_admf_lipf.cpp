// The ADMF's LIPF (ADR-0462 step 4): provisioning a task onto the POIs and the MDF2 over LI_X1, and
// undoing it. The network elements here are in-memory, but they are li_core's REAL X1 server codec
// (handle_request with a recording task store), so every request the LIPF builds is parsed and
// schema-validated exactly as a real NE would, and every answer is a real X1 response.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "li_admf_fakes.hpp"
#include "li_core/x1_server.hpp"
#include "lipf.hpp"

#include <gtest/gtest.h>

namespace {

namespace x1 = li_core::x1;
using li_admf::Destination;
using li_admf::Lipf;
using li_admf::LipfConfig;
using li_admf::NetworkElement;
using li_admf::TaskSpec;

using li_admf_test::FakeNe;
using li_admf_test::FakeTransport;

const std::vector<NetworkElement> kElements = {
    {"amf-poi", "poi", "amf-1", "https://amf:7807/X1/NE"},
    {"mdf2", "mdf2", "mdf2-01", "https://mdf:7805/X1/NE"},
};
constexpr const char* kXid = "2b36a78b-b628-416d-bd22-404e68a0cd36";

LipfConfig config() {
    LipfConfig c;
    c.admf_identifier = "admf-1";
    c.poi_identifier_association = x1::IdentifierAssociationEventsGenerated::All;
    return c;
}

TaskSpec spec() {
    TaskSpec s;
    s.xid = kXid;
    s.liid = "LIID-2026-0042";
    s.targets.push_back({x1::TargetIdentifierKind::SupiImsi, "supiimsi", "999700000000001"});
    s.destinations = {{"192.0.2.10:9443"}};
    s.start_time = "2026-10-06T00:00:00.000000Z";
    s.end_time = "2026-12-01T00:00:00.000000Z";
    return s;
}

TEST(LiAdmfLipf, ProvisionsTheMdf2ThenThePoiWithTheMappedFields) {
    FakeTransport net;
    Lipf lipf(config(), kElements, net);
    const auto r = lipf.provision(spec());
    ASSERT_TRUE(r.ok) << r.detail;

    // MDF2: the destination (deterministic DID), then the task carrying the XID -> LIID mapping.
    const std::string did = Lipf::destination_id(kXid, "192.0.2.10:9443");
    ASSERT_EQ(net.mdf.destinations.count(did), 1U);
    EXPECT_EQ(net.mdf.destinations[did].address.value, "192.0.2.10:9443");
    ASSERT_EQ(net.mdf.tasks.count(kXid), 1U);
    const auto& mt = net.mdf.tasks[kXid];
    ASSERT_EQ(mt.mediation_details.size(), 1U);
    EXPECT_EQ(mt.mediation_details[0].liid, "LIID-2026-0042");
    EXPECT_EQ(mt.mediation_details[0].delivery, x1::MediationDeliveryType::Hi2Only);
    EXPECT_EQ(mt.mediation_details[0].dids, std::vector<std::string>{did});
    EXPECT_EQ(mt.mediation_details[0].end_time, "2026-12-01T00:00:00.000000Z");
    EXPECT_EQ(mt.delivery, x1::DeliveryType::X2Only);
    // Order matters: the destination must exist before the task that references it.
    ASSERT_GE(net.mdf.log.size(), 2U);
    EXPECT_EQ(net.mdf.log[0].rfind("CreateDestination", 0), 0U);
    EXPECT_EQ(net.mdf.log[1].rfind("ActivateTask", 0), 0U);

    // POI: the target identifier, X2 only, with the configured identifier-association gating.
    ASSERT_EQ(net.amf.tasks.count(kXid), 1U);
    const auto& pt = net.amf.tasks[kXid];
    ASSERT_EQ(pt.targets.size(), 1U);
    EXPECT_EQ(pt.targets[0].value, "999700000000001");
    EXPECT_EQ(pt.delivery, x1::DeliveryType::X2Only);
    EXPECT_EQ(pt.identifier_association_events, x1::IdentifierAssociationEventsGenerated::All);
}

TEST(LiAdmfLipf, ANeThatRefusesRollsEverythingBack) {
    FakeTransport net;
    net.amf.fail_activate = x1::ErrorCode::UnsupportedTargetIdentifier; // the POI says no
    Lipf lipf(config(), kElements, net);
    const auto r = lipf.provision(spec());
    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.x1_error, static_cast<int>(x1::ErrorCode::UnsupportedTargetIdentifier));
    EXPECT_NE(r.detail.find("amf-poi"), std::string::npos) << r.detail;
    // All-or-nothing: the MDF2 task and destination this call created are gone again.
    EXPECT_TRUE(net.mdf.tasks.empty());
    EXPECT_TRUE(net.mdf.destinations.empty());
    EXPECT_TRUE(net.amf.tasks.empty());
}

TEST(LiAdmfLipf, InfeasibleTasksAreRefusedBeforeAnyNeIsTouched) {
    FakeTransport net;
    Lipf lipf(config(), kElements, net);

    TaskSpec cc = spec();
    cc.wants_cc = true; // no CC-POI exists: refuse rather than silently downgrade to IRI-only
    auto r = lipf.provision(cc);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("content interception"), std::string::npos) << r.detail;

    TaskSpec imei = spec();
    imei.targets = {{x1::TargetIdentifierKind::Imei, "imei", "490154203237518"}};
    r = lipf.provision(imei);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("imei"), std::string::npos) << r.detail;

    TaskSpec nowhere = spec();
    nowhere.destinations.clear();
    EXPECT_FALSE(lipf.provision(nowhere).ok);

    EXPECT_TRUE(net.amf.log.empty());
    EXPECT_TRUE(net.mdf.log.empty());

    Lipf unconfigured(config(), {}, net);
    EXPECT_FALSE(unconfigured.provision(spec()).ok);
}

TEST(LiAdmfLipf, ReprovisioningAnExistingXidConverges) {
    FakeTransport net;
    Lipf lipf(config(), kElements, net);
    ASSERT_TRUE(lipf.provision(spec()).ok);
    TaskSpec changed = spec();
    changed.liid = "LIID-2026-0099";
    const auto r = lipf.provision(changed); // a retry after a crash, with a corrected LIID
    ASSERT_TRUE(r.ok) << r.detail;
    EXPECT_EQ(net.mdf.tasks[kXid].mediation_details[0].liid, "LIID-2026-0099");
    EXPECT_EQ(net.mdf.destinations.size(), 1U); // no duplicate destination
}

TEST(LiAdmfLipf, DeprovisionRemovesTheTaskEverywhereAndIsIdempotent) {
    FakeTransport net;
    Lipf lipf(config(), kElements, net);
    ASSERT_TRUE(lipf.provision(spec()).ok);
    auto r = lipf.deprovision(spec());
    ASSERT_TRUE(r.ok) << r.detail;
    EXPECT_TRUE(net.amf.tasks.empty());
    EXPECT_TRUE(net.mdf.tasks.empty());
    EXPECT_TRUE(net.mdf.destinations.empty());
    r = lipf.deprovision(spec()); // an XID the NEs no longer know (X1 2020) is "done"
    EXPECT_TRUE(r.ok) << r.detail;
}

TEST(LiAdmfLipf, ChangeDeliveryAddsTheNewDestinationAndRemapsTheMdf2Task) {
    FakeTransport net;
    Lipf lipf(config(), kElements, net);
    ASSERT_TRUE(lipf.provision(spec()).ok);
    TaskSpec moved = spec();
    moved.destinations = {{"198.51.100.7:9443"}};
    const auto r = lipf.change_delivery(moved);
    ASSERT_TRUE(r.ok) << r.detail;
    const std::string new_did = Lipf::destination_id(kXid, "198.51.100.7:9443");
    EXPECT_EQ(net.mdf.destinations.count(new_did), 1U);
    EXPECT_EQ(net.mdf.tasks[kXid].mediation_details[0].dids, std::vector<std::string>{new_did});
}

TEST(LiAdmfLipf, KeepalivesGoToEveryNeAndAFailureIsReportedPerNe) {
    FakeTransport net;
    Lipf lipf(config(), kElements, net);
    auto results = lipf.keepalive_all();
    ASSERT_EQ(results.size(), 2U);
    EXPECT_TRUE(results[0].ok) << results[0].detail;
    EXPECT_TRUE(results[1].ok) << results[1].detail;

    net.amf.keepalive_supported = false; // answers X1 error 1070
    results = lipf.keepalive_all();
    EXPECT_FALSE(results[0].ok);
    EXPECT_EQ(results[0].ne, "amf-poi");
    EXPECT_NE(results[0].detail.find("1070"), std::string::npos) << results[0].detail;
    EXPECT_TRUE(results[1].ok) << "one NE failing must not hide the other";
}

TEST(LiAdmfLipf, TheDestinationIdIsDeterministicAndPerTask) {
    EXPECT_EQ(Lipf::destination_id("a", "1.2.3.4:5"), Lipf::destination_id("a", "1.2.3.4:5"));
    EXPECT_NE(Lipf::destination_id("a", "1.2.3.4:5"), Lipf::destination_id("b", "1.2.3.4:5"));
    EXPECT_NE(Lipf::destination_id("a", "1.2.3.4:5"), Lipf::destination_id("a", "1.2.3.4:6"));
    EXPECT_EQ(Lipf::destination_id("a", "x").size(), 36U); // a UUID, as X1's DId requires
}

} // namespace
