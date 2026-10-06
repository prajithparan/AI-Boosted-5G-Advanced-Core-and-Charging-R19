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

const std::vector<NetworkElement> kElementsWithSmf = {
    {"amf-poi", "poi", "amf-1", "u1", "amf", {"supiimsi", "supinai", "imsi", "nai"}},
    {"smf-poi",
     "poi",
     "smf-1",
     "u3",
     "smf",
     {"supiimsi", "supinai", "peiImei", "peiImeisv", "gpsiMsisdn", "gpsiNai"}},
    {"mdf2", "mdf2", "mdf2-01", "u2", "li-mdf", {}},
};

TEST(LiAdmfLipf, EachPoiIsToldOnlyTheTargetsItCanMatch) {
    FakeTransport net;
    Lipf lipf(config(), kElementsWithSmf, net);
    TaskSpec s = spec();
    s.targets.push_back({x1::TargetIdentifierKind::PeiImei, "peiImei", "49015420323751"});
    s.targets.push_back({x1::TargetIdentifierKind::GpsiMsisdn, "gpsiMsisdn", "447700900123"});
    const auto r = lipf.provision(s);
    ASSERT_TRUE(r.ok) << r.detail;
    // The AMF POI would refuse a PEI/GPSI with 3010; it is never sent one.
    ASSERT_EQ(net.amf.tasks.count(kXid), 1U);
    ASSERT_EQ(net.amf.tasks[kXid].targets.size(), 1U);
    EXPECT_EQ(net.amf.tasks[kXid].targets[0].element, "supiimsi");
    // The SMF POI gets the SUPI, PEI and GPSI.
    ASSERT_EQ(net.smf.tasks.count(kXid), 1U);
    EXPECT_EQ(net.smf.tasks[kXid].targets.size(), 3U);
    // The MDF2 sees the whole warrant (it needs the XID -> LIID mapping, not the matching).
    EXPECT_EQ(net.mdf.tasks[kXid].targets.size(), 3U);
}

TEST(LiAdmfLipf, ATaskOnlyOnePoiCanCarryLeavesTheOthersUntouchedAndAnUncarryableOneIsRefused) {
    FakeTransport net;
    Lipf lipf(config(), kElementsWithSmf, net);
    TaskSpec pei_only = spec();
    pei_only.targets = {{x1::TargetIdentifierKind::PeiImeisv, "peiImeisv", "4901542032375123"}};
    ASSERT_TRUE(lipf.provision(pei_only).ok);
    EXPECT_TRUE(net.amf.log.empty()) << "the AMF POI was contacted for a target it cannot match";
    EXPECT_EQ(net.smf.tasks.count(kXid), 1U);

    // A plain IMEI element (not one of the SMF's PEI formats) is carried by no POI.
    TaskSpec imei = spec();
    imei.xid = "99999999-9999-4999-8999-999999999999";
    imei.targets = {{x1::TargetIdentifierKind::Imei, "imei", "49015420323751"}};
    const auto r = lipf.provision(imei);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.detail.find("imei"), std::string::npos) << r.detail;
}

TEST(LiAdmfLipf, TheHi1FormatForAnX1ElementIsTheAnnexCName) {
    EXPECT_EQ(li_admf::hi1_format_for_element("peiImei"), "PEIIMEI");
    EXPECT_EQ(li_admf::hi1_format_for_element("gpsiMsisdn"), "GPSIMSISDN");
    EXPECT_EQ(li_admf::hi1_format_for_element("supiimsi"), "SUPIIMSI");
    EXPECT_FALSE(li_admf::hi1_format_for_element("imei").has_value());
}

std::vector<NetworkElement> cc_elements() {
    NetworkElement smf{"smf-poi",
                       "poi",
                       "smf-1",
                       "u3",
                       "smf",
                       {"supiimsi", "supinai", "peiImei", "peiImeisv", "gpsiMsisdn", "gpsiNai"}};
    smf.cc_capable = true;
    return {{"amf-poi", "poi", "amf-1", "u1", "amf", {"supiimsi", "supinai", "imsi", "nai"}},
            smf,
            {"mdf2", "mdf2", "mdf2-01", "u2", "li-mdf", {}},
            {"mdf3", "mdf3", "mdf3-01", "u4", "li-mdf3", {}}};
}

LipfConfig cc_config() {
    LipfConfig c = config();
    c.cc_capable = true;
    return c;
}

TEST(LiAdmfLipf, ACcAndIriTaskGoesToTheMdf3AndTheCcCapablePoiAsWellAsTheMdf2) {
    FakeTransport net;
    Lipf lipf(cc_config(), cc_elements(), net);
    TaskSpec s = spec();
    s.wants_cc = true;
    const auto r = lipf.provision(s);
    ASSERT_TRUE(r.ok) << r.detail;

    const std::string did = Lipf::destination_id(kXid, "192.0.2.10:9443");
    // IRI: the AMF POI streams X2; the MDF2 mediates HI2 only.
    EXPECT_EQ(net.amf.tasks[kXid].delivery, x1::DeliveryType::X2Only);
    EXPECT_EQ(net.mdf.tasks[kXid].delivery, x1::DeliveryType::X2Only);
    EXPECT_EQ(net.mdf.tasks[kXid].mediation_details.at(0).delivery,
              x1::MediationDeliveryType::Hi2Only);
    EXPECT_EQ(net.mdf.destinations[did].delivery, x1::DeliveryType::X2Only);
    // CC: the SMF POI does both (it triggers the UPF), the MDF3 mediates HI3 only, with the LIID.
    EXPECT_EQ(net.smf.tasks[kXid].delivery, x1::DeliveryType::X2AndX3);
    EXPECT_EQ(net.mdf3.tasks[kXid].delivery, x1::DeliveryType::X3Only);
    ASSERT_EQ(net.mdf3.tasks[kXid].mediation_details.size(), 1U);
    EXPECT_EQ(net.mdf3.tasks[kXid].mediation_details[0].liid, "LIID-2026-0042");
    EXPECT_EQ(net.mdf3.tasks[kXid].mediation_details[0].delivery,
              x1::MediationDeliveryType::Hi3Only);
    EXPECT_EQ(net.mdf3.destinations[did].delivery, x1::DeliveryType::X3Only);

    // Deprovisioning removes it from the MDF3 too.
    ASSERT_TRUE(lipf.deprovision(s).ok);
    EXPECT_TRUE(net.mdf3.tasks.empty());
    EXPECT_TRUE(net.mdf3.destinations.empty());
    EXPECT_TRUE(net.smf.tasks.empty());
}

TEST(LiAdmfLipf, ACcOnlyTaskLeavesTheIriPathsUntouched) {
    FakeTransport net;
    Lipf lipf(cc_config(), cc_elements(), net);
    TaskSpec s = spec();
    s.wants_iri = false;
    s.wants_cc = true;
    ASSERT_TRUE(lipf.provision(s).ok);
    EXPECT_TRUE(net.amf.tasks.empty());
    EXPECT_TRUE(net.mdf.tasks.empty());
    EXPECT_TRUE(net.mdf.destinations.empty());
    EXPECT_EQ(net.smf.tasks[kXid].delivery, x1::DeliveryType::X3Only);
    EXPECT_EQ(net.mdf3.tasks.count(kXid), 1U);
}

TEST(LiAdmfLipf, CcIsRefusedUnlessTheDeploymentCanReallyDeliverIt) {
    FakeTransport net;
    TaskSpec s = spec();
    s.wants_cc = true;
    // The master switch is off (no real packet capture): refused, not downgraded to IRI.
    EXPECT_NE(Lipf(config(), cc_elements(), net).infeasible(s)->find("cc_capable"),
              std::string::npos);
    // No MDF3.
    auto without_mdf3 = cc_elements();
    without_mdf3.pop_back();
    EXPECT_NE(Lipf(cc_config(), without_mdf3, net).infeasible(s)->find("MDF3"), std::string::npos);
    // A target only an IRI-only POI carries (the AMF's `imsi`) cannot be intercepted for content.
    TaskSpec imsi = spec();
    imsi.wants_cc = true;
    imsi.targets = {{x1::TargetIdentifierKind::Imsi, "imsi", "999700000000001"}};
    const auto why = Lipf(cc_config(), cc_elements(), net).infeasible(imsi);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("imsi"), std::string::npos);
    // The same warrant for IRI alone is fine.
    imsi.wants_cc = false;
    EXPECT_FALSE(Lipf(cc_config(), cc_elements(), net).infeasible(imsi).has_value());
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
