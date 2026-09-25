// ADR-0384: the TMF620 stores now persist the NORMALIZED product_catalog schema of the consolidated
// charging DB. "Normalized" must not mean "lossy": these tests post FULLY POPULATED resources --
// every DTO field set, every list with at least two elements, TMF-local ids deliberately reused
// across owners -- and require get() and list() to return exactly what was posted (with id/href
// assigned by the server). Dates are posted already in the canonical form the store returns
// (UTC, milliseconds), so equality is exact; canonicalisation itself is asserted separately.
//
// Real PostgreSQL (TEST_POSTGRES_URL, the charging DB); skipped when none is reachable, like
// test_product_catalog_postgres.cpp.

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <pqxx/pqxx>
#include <string>

#include "store.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

std::string conninfo() {
    if (const char* env = std::getenv("TEST_POSTGRES_URL")) {
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

constexpr const char* kBase = "https://test/tmf-api/productCatalogManagement/v4";
constexpr const char* kT1 = "2026-01-02T03:04:05.000Z";
constexpr const char* kT2 = "2027-06-30T23:59:59.999Z";

bss_sid::TimePeriod period() {
    return bss_sid::TimePeriod{kT1, kT2};
}

bss_sid::CharacteristicValueSpecification value_spec(const std::string& tag, bool is_default) {
    bss_sid::CharacteristicValueSpecification v;
    v.isDefault = is_default;
    v.rangeInterval = "closed";
    v.regex = "^[0-9]+$";
    v.unitOfMeasure = bss_sid::Quantity{1.5, "GB"};
    v.valueFrom = "1";
    v.valueTo = "100";
    v.valueType = "integer";
    v.validFor = period();
    v.value = json{{"tag", tag}, {"n", 42}};
    return v;
}

// Same TMF id ("ratingGroup") on every owner: TMF620 scopes it to its owner, the DB must too.
bss_sid::ProductSpecificationCharacteristicValueUse value_use(const std::string& spec_id,
                                                              const std::string& name) {
    bss_sid::ProductSpecificationCharacteristicValueUse u;
    u.id = "ratingGroup";
    u.description = "desc-" + name;
    u.maxCardinality = 3;
    u.minCardinality = 1;
    u.name = name;
    u.valueType = "integer";
    u.productSpecCharacteristicValue = {value_spec(name + "-a", true),
                                        value_spec(name + "-b", false)};
    u.productSpecification = bss_sid::ProductSpecificationRef{
        spec_id, "https://x/spec/" + spec_id, "Spec", "1.0", "https://schema/ProductSpec"};
    u.validFor = period();
    return u;
}

bss_sid::AttachmentRefOrValue attachment(const std::string& tag) {
    bss_sid::AttachmentRefOrValue a;
    a.id = "att-" + tag;
    a.href = "https://x/att/" + tag;
    a.attachmentType = "brochure";
    a.content = "Y29udGVudA==";
    a.description = "d-" + tag;
    a.mimeType = "application/pdf";
    a.name = "n-" + tag;
    a.url = "https://x/files/" + tag;
    a.size = bss_sid::Quantity{2.0, "MB"};
    a.validFor = period();
    return a;
}

bss_sid::ProductSpecification full_spec() {
    bss_sid::ProductSpecification sp;
    sp.brand = "Lab";
    sp.description = "Full spec";
    sp.isBundle = false;
    sp.lifecycleStatus = "Active";
    sp.lastUpdate = kT1;
    sp.name = "Lossless Spec";
    sp.productNumber = "PN-1";
    sp.version = "2.1";
    for (const char* tag : {"rg", "slice"}) {
        bss_sid::ProductSpecificationCharacteristic c;
        c.id = std::string("char-") + tag;
        c.configurable = true;
        c.description = std::string("about ") + tag;
        c.extensible = false;
        c.isUnique = true;
        c.maxCardinality = 2;
        c.minCardinality = 0;
        c.name = tag;
        c.regex = ".*";
        c.valueType = "string";
        c.productSpecCharacteristicValue = {value_spec(tag, true), value_spec(tag, false)};
        c.validFor = period();
        sp.productSpecCharacteristic.push_back(c);
    }
    sp.attachment = {attachment("s1"), attachment("s2")};
    sp.bundledProductSpecification = {{"bs-1", "https://x/bs/1", "Active", "B1"},
                                      {"bs-2", "https://x/bs/2", "Retired", "B2"}};
    sp.productSpecificationRelationship = {
        {"rel-1", "https://x/r/1", "R1", "dependency", period()},
        {"rel-2", "https://x/r/2", "R2", "exclusivity", period()}};
    sp.relatedParty = {{"party-1", "https://x/p/1", "Owner", "owner"},
                       {"party-2", "https://x/p/2", "Vendor", "supplier"}};
    sp.resourceSpecification = {{"rs-1", "https://x/rs/1", "RS1", "1"},
                                {"rs-2", "https://x/rs/2", "RS2", "2"}};
    sp.serviceSpecification = {{"ss-1", "https://x/ss/1", "SS1", "1"},
                               {"ss-2", "https://x/ss/2", "SS2", "2"}};
    sp.targetProductSchema = json{{"@type", "LabProduct"}, {"@schemaLocation", "https://s/x.json"}};
    sp.validFor = period();
    return sp;
}

bss_sid::ProductOfferingPrice full_price(const std::string& spec_id, const std::string& name) {
    bss_sid::ProductOfferingPrice p;
    p.name = name;
    p.description = "per-octet usage price";
    p.lifecycleStatus = "Active";
    p.lastUpdate = kT1;
    p.priceType = "usage";
    p.percentage = 12.5;
    p.version = "3";
    p.price = bss_sid::Money{"USD", 0.0000123456789}; // sub-cent: NUMERIC(18,4) would round it
    p.recurringChargePeriodLength = 1;
    p.recurringChargePeriodType = "month";
    p.unitOfMeasure = bss_sid::Quantity{1.0, "octet"};
    p.prodSpecCharValueUse = {value_use(spec_id, "rgA"), value_use(spec_id, "rgB")};
    p.bundledPopRelationship = {{"bpop-1", "https://x/bpop/1", "BP1"},
                                {"bpop-2", "https://x/bpop/2", "BP2"}};
    p.constraint = {{"c-1", "https://x/c/1", "C1", "1"}, {"c-2", "https://x/c/2", "C2", "2"}};
    p.place = {{"pl-1", "https://x/pl/1", "Place1"}, {"pl-2", "https://x/pl/2", "Place2"}};
    p.popRelationship = {{"pop-1", "https://x/pop/1", "P1", "discount", "target"},
                         {"pop-2", "https://x/pop/2", "P2", "alteration", "source"}};
    p.pricingLogicAlgorithm = {{"pla-1", "https://x/pla/1", "d1", "PLA1", "spec-1", period()},
                               {"pla-2", "https://x/pla/2", "d2", "PLA2", "spec-2", period()}};
    p.productOfferingTerm = {{"t1", "Term1", bss_sid::Duration{12, "month"}, period()},
                             {"t2", "Term2", bss_sid::Duration{24, "month"}, period()}};
    p.tax = {{"tax-1", "https://x/tax/1", "VAT", 20.0, bss_sid::Money{"USD", 0.25}},
             {"tax-2", "https://x/tax/2", "GST", 5.0, bss_sid::Money{"USD", 0.0625}}};
    p.validFor = period();
    return p;
}

bss_sid::ProductOffering
full_offering(const std::string& spec_id, const std::string& price_a, const std::string& price_b) {
    bss_sid::ProductOffering o;
    o.name = "Lossless Offering";
    o.description = "every field set";
    o.lifecycleStatus = "Active";
    o.lastUpdate = kT1;
    o.statusReason = "launched";
    o.isBundle = true;
    o.isSellable = true;
    o.version = "1.2";
    o.productOfferingPrice = {
        {price_a, std::string(kBase) + "/productOfferingPrice/" + price_a, "PA"},
        {price_b, std::string(kBase) + "/productOfferingPrice/" + price_b, "PB"}};
    o.category = {{"cat-1", "https://x/cat/1", "Cat1", "1"},
                  {"cat-2", "https://x/cat/2", "Cat2", "2"}};
    o.channel = {{"ch-1", "https://x/ch/1", "Web"}, {"ch-2", "https://x/ch/2", "Retail"}};
    o.marketSegment = {{"ms-1", "https://x/ms/1", "Consumer"}, {"ms-2", "https://x/ms/2", "SME"}};
    o.prodSpecCharValueUse = {value_use(spec_id, "offA"), value_use(spec_id, "offB")};
    o.productSpecification = bss_sid::ProductSpecificationRef{
        spec_id, "https://x/spec/" + spec_id, "Lossless Spec", "2.1", "https://schema/PS"};
    o.resourceCandidate = bss_sid::ResourceCandidateRef{"rc-1", "https://x/rc/1", "RC", "1"};
    o.serviceCandidate = bss_sid::ServiceCandidateRef{"sc-1", "https://x/sc/1", "SC", "1"};
    o.serviceLevelAgreement = bss_sid::SLARef{"sla-1", "https://x/sla/1", "Gold"};
    o.agreement = {{"agr-1", "https://x/agr/1", "A1"},
                   {"agr-2", "https://x/agr/2", "A2"},
                   {"agr-3", "https://x/agr/3", "A3"}}; // three: the old schema held one
    o.bundledProductOffering = {{"bo-1", "https://x/bo/1", "Active", "BO1"},
                                {"bo-2", "https://x/bo/2", "Active", "BO2"}};
    o.attachment = {attachment("o1"), attachment("o2")};
    o.place = {{"pl-1", "https://x/pl/1", "Place1"}, {"pl-2", "https://x/pl/2", "Place2"}};
    o.productOfferingRelationship = {
        {"orel-1", "https://x/orel/1", "OR1", "requires", "parent", period()},
        {"orel-2", "https://x/orel/2", "OR2", "excludes", "child", period()}};
    o.productOfferingTerm = {{"t1", "Term1", bss_sid::Duration{12, "month"}, period()},
                             {"t2", "Term2", bss_sid::Duration{1, "year"}, period()}};
    o.validFor = period();
    return o;
}

template <typename T> json with_identity(T v, const std::string& id, const std::string& coll) {
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

class ProductCatalogLossless : public ::testing::Test {
protected:
    void SetUp() override {
        if (!reachable()) {
            GTEST_SKIP() << "No reachable charging DB at " << conninfo();
        }
    }
    product_catalog::ProductSpecificationStore specs{
        std::string(kBase) + "/productSpecification", conninfo(), 1};
    product_catalog::ProductOfferingPriceStore prices{
        std::string(kBase) + "/productOfferingPrice", conninfo(), 1};
    product_catalog::ProductOfferingStore offerings{
        std::string(kBase) + "/productOffering", conninfo(), 1};
};

} // namespace

TEST_F(ProductCatalogLossless, FullyPopulatedResourcesRoundTripThroughGetAndList) {
    const auto spec_in = full_spec();
    const auto spec_id = specs.create(spec_in);
    const auto spec_expected = with_identity(spec_in, spec_id, "productSpecification");
    EXPECT_EQ(json(*specs.get(spec_id)), spec_expected);
    EXPECT_EQ(find_in(specs.list(), spec_id), spec_expected);

    const auto price_a_in = full_price(spec_id, "PA");
    const auto price_a = prices.create(price_a_in);
    const auto price_b = prices.create(full_price(spec_id, "PB"));
    const auto price_expected = with_identity(price_a_in, price_a, "productOfferingPrice");
    EXPECT_EQ(json(*prices.get(price_a)), price_expected);
    EXPECT_EQ(find_in(prices.list(), price_a), price_expected);

    const auto off_in = full_offering(spec_id, price_a, price_b);
    const auto off_id = offerings.create(off_in);
    const auto off_expected = with_identity(off_in, off_id, "productOffering");
    EXPECT_EQ(json(*offerings.get(off_id)), off_expected);
    EXPECT_EQ(find_in(offerings.list(), off_id), off_expected);

    // Clean up in dependency order (offering -> prices -> spec); each delete must succeed.
    EXPECT_TRUE(offerings.remove(off_id));
    EXPECT_TRUE(prices.remove(price_a));
    EXPECT_TRUE(prices.remove(price_b));
    EXPECT_TRUE(specs.remove(spec_id));
    EXPECT_FALSE(specs.get(spec_id).has_value());
}

TEST_F(ProductCatalogLossless, DateTimesAreCanonicalisedAndMalformedOnesRejected) {
    bss_sid::ProductOffering o;
    o.name = "tz";
    o.lastUpdate = "2026-03-04T10:00:00+05:30"; // offset form
    const auto id = offerings.create(o);
    EXPECT_EQ(offerings.get(id)->lastUpdate, "2026-03-04T04:30:00.000Z");
    EXPECT_TRUE(offerings.remove(id));

    o.lastUpdate = "yesterday";
    EXPECT_THROW(offerings.create(o), product_catalog::InvalidRequest);
}

TEST_F(ProductCatalogLossless, ReferentialIntegrityIsA400OnWriteAndA409OnDelete) {
    bss_sid::ProductOffering nameless;
    EXPECT_THROW(offerings.create(nameless), product_catalog::InvalidRequest);

    bss_sid::ProductOffering dangling;
    dangling.name = "dangling";
    dangling.productOfferingPrice = {{"no-such-price", std::nullopt, std::nullopt}};
    EXPECT_THROW(offerings.create(dangling), product_catalog::InvalidRequest);

    bss_sid::ProductOfferingPrice price;
    price.name = "in use";
    const auto price_id = prices.create(price);
    bss_sid::ProductOffering uses;
    uses.name = "uses a price";
    uses.productOfferingPrice = {{price_id, std::nullopt, std::nullopt}};
    const auto off_id = offerings.create(uses);
    EXPECT_THROW(prices.remove(price_id), product_catalog::Conflict);
    EXPECT_TRUE(prices.get(price_id).has_value()) << "a refused delete must not delete";

    EXPECT_TRUE(offerings.remove(off_id));
    EXPECT_TRUE(prices.remove(price_id));
    EXPECT_FALSE(prices.remove(price_id));
}
