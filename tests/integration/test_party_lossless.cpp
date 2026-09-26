// ADR-0386: TMF632 Individual / Organization persisted in the normalized `party` schema of the
// consolidated charging DB. Fully populated parties -- every DTO field, every list with at least
// two elements, date-times already in the canonical form the store returns -- must come back
// exactly through get() and list(). Plus the constraints the relational model now enforces.
//
// Real PostgreSQL (TEST_SUBSCRIBER_MANAGEMENT_POSTGRES_URL, the charging DB); skipped without one.

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <ctime>
#include <pqxx/pqxx>
#include <string>

#include "../../bss/subscriber-management/src/store.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

std::string conninfo() {
    if (const char* env = std::getenv("TEST_SUBSCRIBER_MANAGEMENT_POSTGRES_URL")) {
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

constexpr const char* kBase = "https://test/tmf-api/party/v4";
constexpr const char* kT1 = "1990-05-06T07:08:09.010Z";
constexpr const char* kT2 = "2030-01-01T00:00:00.000Z";

bss_sid::TimePeriod period() {
    return bss_sid::TimePeriod{kT1, kT2};
}

bss_sid::AttachmentRefOrValue attachment(const std::string& tag) {
    bss_sid::AttachmentRefOrValue a;
    a.id = "att-" + tag;
    a.name = "doc-" + tag;
    a.mimeType = "application/pdf";
    a.url = "https://x/docs/" + tag;
    return a;
}

template <typename Party> void populate_shared(Party& p) {
    for (int i = 0; i < 2; ++i) {
        const auto n = std::to_string(i);
        bss_sid::ContactMedium c;
        c.mediumType = i == 0 ? "email" : "postalAddress";
        c.preferred = i == 0;
        c.characteristic = bss_sid::MediumCharacteristic{"City" + n,
                                                         "home",
                                                         "IN",
                                                         "a" + n + "@x.org",
                                                         "fax" + n,
                                                         "+9100" + n,
                                                         "600" + n,
                                                         "sn" + n,
                                                         "TN"};
        c.validFor = period();
        p.contactMedium.push_back(c);
        p.creditRating.push_back({"Agency" + n, "external", "ref" + n, 700 + i, period()});
        p.externalReference.push_back({"crm", "ext-" + n});
        p.partyCharacteristic.push_back(
            {"segment" + n, "string", json{{"v", n}, {"nested", json::array({1, 2})}}});
        p.relatedParty.push_back({"rp-" + n, "https://x/rp/" + n, "Rel" + n, "guardian"});
        bss_sid::TaxExemptionCertificate t;
        t.id = "cert-" + n;
        t.attachment = attachment("tax" + n);
        t.taxDefinition = {{"td-" + n + "a", "VAT", "vat"}, {"td-" + n + "b", "GST", "gst"}};
        t.validFor = period();
        p.taxExemptionCertificate.push_back(t);
    }
}

bss_sid::Individual full_individual() {
    bss_sid::Individual v;
    v.aristocraticTitle = "Dr";
    v.birthDate = kT1;
    v.countryOfBirth = "IN";
    v.deathDate = kT2;
    v.familyName = "Kumar";
    v.familyNamePrefix = "van";
    v.formattedName = "Dr A Kumar";
    v.fullName = "Arun Kumar";
    v.gender = "male";
    v.generation = "Jr";
    v.givenName = "Arun";
    v.legalName = "Arun Kumar";
    v.location = "Chennai";
    v.maritalStatus = "married";
    v.middleName = "K";
    v.nationality = "IN";
    v.placeOfBirth = "Madurai";
    v.preferredGivenName = "AK";
    v.title = "Mr";
    v.status = "validated";
    populate_shared(v);
    for (int i = 0; i < 2; ++i) {
        const auto n = std::to_string(i);
        v.disability.push_back({"D" + n, "dis" + n, period()});
        v.individualIdentification.push_back(
            {"passport", "P" + n, "Gov", kT1, attachment("id" + n), period()});
        v.languageAbility.push_back({i == 0, "ta", "Tamil", "C2", "C1", "B2", "B1", period()});
        v.otherName.push_back({"Sir",
                               "K" + n,
                               "de",
                               "F" + n,
                               "Full" + n,
                               "II",
                               "G" + n,
                               "L" + n,
                               "M" + n,
                               "P" + n,
                               "T" + n,
                               period()});
        v.skill.push_back({"c" + n, "expert", "S" + n, "skill" + n, period()});
    }
    return v;
}

bss_sid::Organization full_organization(const std::optional<std::string>& parent_id) {
    bss_sid::Organization v;
    v.isHeadOffice = true;
    v.isLegalEntity = true;
    v.name = "Acme Telecom";
    v.nameType = "legal";
    v.organizationType = "company";
    v.tradingName = "Acme";
    v.existsDuring = period();
    v.status = "validated";
    populate_shared(v);
    for (int i = 0; i < 2; ++i) {
        const auto n = std::to_string(i);
        v.organizationIdentification.push_back(
            {"regNo", "R" + n, "ROC", kT1, attachment("org" + n), period()});
        v.otherName.push_back({"Acme" + n, "trading", "AcmeT" + n, period()});
        v.organizationChildRelationship.push_back(
            {"branch", bss_sid::OrganizationRef{"child-" + n, "https://x/org/c" + n, "Child" + n}});
    }
    if (parent_id.has_value()) {
        v.organizationParentRelationship = bss_sid::OrganizationParentRelationship{
            "subsidiary", bss_sid::OrganizationRef{*parent_id, "https://x/org/p", "Parent"}};
    }
    return v;
}

template <typename T> json expected(T v, const std::string& id, const std::string& coll) {
    v.id = id;
    v.href = std::string(kBase) + "/" + coll + "/" + id;
    return json(v);
}

template <typename T> json find_in(const std::vector<T>& all, const std::string& id) {
    for (const auto& x : all) {
        if (x.id == id) {
            return json(x);
        }
    }
    return json();
}

class PartyLossless : public ::testing::Test {
protected:
    void SetUp() override {
        if (!reachable()) {
            GTEST_SKIP() << "No reachable charging DB at " << conninfo();
        }
    }
};

} // namespace

TEST_F(PartyLossless, FullyPopulatedIndividualRoundTrips) {
    subscriber_management::PartyIndividualStore store(std::string(kBase) + "/individual",
                                                      conninfo());
    const auto in = full_individual();
    const auto id = store.create(in);
    const auto want = expected(in, id, "individual");
    EXPECT_EQ(json(*store.get(id)), want);
    EXPECT_EQ(find_in(store.list(), id), want);
}

TEST_F(PartyLossless, FullyPopulatedOrganizationWithParentRoundTrips) {
    subscriber_management::PartyOrganizationStore store(std::string(kBase) + "/organization",
                                                        conninfo());
    const auto parent = store.create(full_organization(std::nullopt));
    const auto in = full_organization(parent);
    const auto id = store.create(in);
    const auto want = expected(in, id, "organization");
    EXPECT_EQ(json(*store.get(id)), want);
    EXPECT_EQ(find_in(store.list(), id), want);
}

TEST_F(PartyLossless, IntegrityViolationsAreClientErrors) {
    subscriber_management::PartyOrganizationStore orgs(std::string(kBase) + "/organization",
                                                       conninfo());
    // A parent organization that does not exist.
    EXPECT_THROW(orgs.create(full_organization(std::string("no-such-org"))),
                 subscriber_management::InvalidRequest);

    subscriber_management::AccountStore accounts("https://test/acc", conninfo());
    subscriber_management::Account bad{};
    bad.accountKind = "WHOLESALE"; // not CONSUMER | ENTERPRISE
    EXPECT_THROW(accounts.create(bad), subscriber_management::InvalidRequest);

    subscriber_management::SubscriberStore subs("https://test/sub", conninfo());
    subscriber_management::Subscriber orphan{};
    orphan.supi = "imsi-99970" + std::to_string(std::time(nullptr) % 1000000000LL) + "7";
    orphan.chargingMode = "PREPAID";
    EXPECT_THROW(subs.create(orphan), subscriber_management::InvalidRequest)
        << "a subscriber without an account";
    orphan.accountId = "no-such-account";
    EXPECT_THROW(subs.create(orphan), subscriber_management::InvalidRequest);

    subscriber_management::PartyIndividualStore people(std::string(kBase) + "/individual",
                                                       conninfo());
    bss_sid::Individual dated;
    dated.birthDate = "sometime";
    EXPECT_THROW(people.create(dated), subscriber_management::InvalidRequest);
}
