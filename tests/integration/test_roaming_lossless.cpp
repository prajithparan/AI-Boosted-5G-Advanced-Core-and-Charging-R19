// ADR-0387: interconnect agreements = TMF651 Agreement normalized in subscriber_mgmt.agreement* +
// roaming specifics in roaming.interconnect_agreement, one shared id. A fully populated agreement
// -- every field, every list >= 2 including each AgreementItem's product / productOffering /
// termOrCondition lists -- must come back exactly through get() and list(); rateTerms omitted stays
// omitted; TAP3 payload bytes round-trip; references the model rejects are client errors.
//
// Real PostgreSQL (TEST_ROAMING_INTERCONNECT_POSTGRES_URL, the charging DB); skipped without one.

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <pqxx/pqxx>
#include <string>

#include "../../bss/roaming-interconnect/src/store.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

std::string conninfo() {
    if (const char* env = std::getenv("TEST_ROAMING_INTERCONNECT_POSTGRES_URL")) {
        return env;
    }
    return "postgresql://postgres@127.0.0.1:5434/charging";
}

bool reachable() {
    try {
        pqxx::connection c(conninfo());
        return c.is_open();
    } catch (const std::exception&) {
        return false;
    }
}

constexpr const char* kBase = "https://test/bss-api/roaming/v1/interconnectAgreement";
constexpr const char* kT1 = "2026-01-01T00:00:00.000Z";
constexpr const char* kT2 = "2027-12-31T23:59:59.999Z";

roaming_interconnect::InterconnectAgreement full_agreement() {
    roaming_interconnect::InterconnectAgreement v;
    v.partnerOperatorPlmnId = "31026";
    v.rateTerms = json{{"perMbUsd", 0.0125}, {"tiers", json::array({1, 2, 3})}};
    auto& a = v.agreement;
    a.agreementType = "commercial";
    a.description = "IOT roaming";
    a.documentNumber = 42;
    a.initialDate = kT1;
    a.name = "Lossless Roaming Deal";
    a.statementOfIntent = "mutual roaming";
    a.status = "approved";
    a.version = "3";
    a.agreementPeriod = bss_sid::TimePeriod{kT1, kT2};
    a.completionDate = bss_sid::TimePeriod{kT1, kT2};
    a.agreementSpecification =
        bss_sid::AgreementSpecificationRef{"spec-1", "https://x/spec/1", "IR.21 template", "Spec"};
    for (int i = 0; i < 2; ++i) {
        const auto n = std::to_string(i);
        a.agreementAuthorization.push_back({kT1, "sig-" + n, "approved"});
        a.associatedAgreement.push_back({"assoc-" + n, "https://x/agr/" + n, "Assoc" + n});
        a.characteristic.push_back({"c" + n, "string", json{{"k", n}}});
        a.engagedParty.push_back({"party-" + n, "https://x/p/" + n, "Operator" + n, "partner"});
        bss_sid::AgreementItem item;
        for (int k = 0; k < 2; ++k) {
            const auto m = n + std::to_string(k);
            item.product.push_back({"prod-" + m, "https://x/prod/" + m, "P" + m});
            item.productOffering.push_back({"off-" + m, "https://x/off/" + m, "O" + m});
            item.termOrCondition.push_back(
                {"term-" + m, "term " + m, bss_sid::TimePeriod{kT1, kT2}});
        }
        a.agreementItem.push_back(item);
    }
    return v;
}

json expected(roaming_interconnect::InterconnectAgreement v, const std::string& id) {
    v.id = id;
    v.href = std::string(kBase) + "/" + id;
    v.agreement.id = id;
    v.agreement.href = v.href;
    return json(v);
}

class RoamingLossless : public ::testing::Test {
protected:
    void SetUp() override {
        if (!reachable()) {
            GTEST_SKIP() << "No reachable charging DB at " << conninfo();
        }
    }
};

} // namespace

TEST_F(RoamingLossless, FullyPopulatedInterconnectAgreementRoundTrips) {
    roaming_interconnect::InterconnectAgreementStore store(kBase, conninfo());
    const auto in = full_agreement();
    const auto id = store.create(in);
    const auto want = expected(in, id);
    EXPECT_EQ(json(*store.get(id)), want);
    json listed;
    for (const auto& a : store.list()) {
        if (a.id == id) {
            listed = json(a);
        }
    }
    EXPECT_EQ(listed, want);
}

TEST_F(RoamingLossless, OmittedRateTermsStayOmitted) {
    roaming_interconnect::InterconnectAgreementStore store(kBase, conninfo());
    roaming_interconnect::InterconnectAgreement v;
    v.agreement.name = "no terms";
    const auto id = store.create(v);
    EXPECT_FALSE(json(*store.get(id)).contains("rateTerms"));
}

TEST_F(RoamingLossless, Tap3PayloadRoundTripsAndDanglingAgreementIsRejected) {
    roaming_interconnect::InterconnectAgreementStore agreements(kBase, conninfo());
    roaming_interconnect::InterconnectAgreement v;
    v.agreement.name = "tap3 owner";
    const auto agreement_id = agreements.create(v);

    roaming_interconnect::RoamingCdrFileStore files(conninfo());
    roaming_interconnect::RoamingCdrFile file;
    file.agreementId = agreement_id;
    file.format = "TAP3";
    for (int b : {0x61, 0x00, 0xff, 0x10, 0x7f}) {
        file.rawPayload.push_back(static_cast<std::byte>(b));
    }
    const auto file_id = files.create(file);
    const auto back = files.get(file_id);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->agreementId, agreement_id);
    EXPECT_EQ(back->format, "TAP3");
    EXPECT_EQ(back->rawPayload, file.rawPayload);

    file.agreementId = "no-such-agreement";
    EXPECT_THROW(files.create(file), roaming_interconnect::InvalidRequest);

    roaming_interconnect::InterconnectAgreement bad;
    bad.agreement.initialDate = "not-a-date";
    EXPECT_THROW(agreements.create(bad), roaming_interconnect::InvalidRequest);
}
