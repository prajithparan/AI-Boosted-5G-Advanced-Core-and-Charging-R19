// ADR-0303 (C7): the predicate that decides whether a product applies to a charging request.
//
// User requirement this implements: "10GB is usable on Slice ID 1 OR 5GB on Slice ID 10 OR UPF ID
// 5. Ideally any attribute coming to CHF on N40/N28 shall be used for Charging and model product."
// So these tests are written as the product statements they encode, not as JSON trivia.

#include "charging_engine.hpp"

#include <gtest/gtest.h>

using nlohmann::json;

namespace {
// A request on slice {sst:1, sd:000001}, DNN internet, served by UPF upf-5, home PLMN 999/70.
json request_attributes() {
    return json{
        {"ratingGroup", 10},
        {"uPFID", "upf-5"},
        {"dnnId", "internet"},
        {"sNSSAI", json{{"sst", 1}, {"sd", "000001"}}},
        {"hPlmnId", json{{"mcc", "999"}, {"mnc", "70"}}},
        {"servingCNPlmnId", json{{"mcc", "999"}, {"mnc", "70"}}},
    };
}
} // namespace

TEST(ChargingScope, AnUnscopedOfferingStillMatchesEverything) {
    // The compatibility guarantee: every offering configured before this feature existed keeps
    // behaving exactly as it did. If this ever fails, the change is not backward compatible.
    EXPECT_TRUE(chf::charging_scope_matches(json::object(), request_attributes()));
    EXPECT_TRUE(chf::charging_scope_matches(json(nullptr), request_attributes()));
}

TEST(ChargingScope, ASliceScopedBundleAppliesOnlyToThatSlice) {
    const json on_slice_1 = {{"sNSSAI", json{{"sst", 1}, {"sd", "000001"}}}};
    EXPECT_TRUE(chf::charging_scope_matches(on_slice_1, request_attributes()));

    const json on_slice_10 = {{"sNSSAI", json{{"sst", 10}, {"sd", "00000a"}}}};
    EXPECT_FALSE(chf::charging_scope_matches(on_slice_10, request_attributes()))
        << "a bundle sold for slice 10 must not be consumed by traffic on slice 1";
}

TEST(ChargingScope, OneBundleCanCoverSeveralSlicesViaAnArray) {
    // "usable on slice 1 or slice 10" as ONE product rather than two -- an array means any-of.
    const json either = {
        {"sNSSAI",
         json::array({json{{"sst", 1}, {"sd", "000001"}}, json{{"sst", 10}, {"sd", "00000a"}}})}};
    EXPECT_TRUE(chf::charging_scope_matches(either, request_attributes()));

    const json neither = {
        {"sNSSAI",
         json::array({json{{"sst", 2}, {"sd", "000002"}}, json{{"sst", 3}, {"sd", "000003"}}})}};
    EXPECT_FALSE(chf::charging_scope_matches(neither, request_attributes()));
}

TEST(ChargingScope, AUpfScopedBundleAppliesOnlyOnThatUpf) {
    EXPECT_TRUE(chf::charging_scope_matches(json{{"uPFID", "upf-5"}}, request_attributes()));
    EXPECT_FALSE(chf::charging_scope_matches(json{{"uPFID", "upf-9"}}, request_attributes()));
}

TEST(ChargingScope, EveryConstraintMustHoldNotJustOne) {
    // Slice matches, UPF does not -> no match. A product scoped on two things means both.
    const json slice_and_wrong_upf = {
        {"sNSSAI", json{{"sst", 1}, {"sd", "000001"}}},
        {"uPFID", "upf-9"},
    };
    EXPECT_FALSE(chf::charging_scope_matches(slice_and_wrong_upf, request_attributes()));

    const json slice_and_right_upf = {
        {"sNSSAI", json{{"sst", 1}, {"sd", "000001"}}},
        {"uPFID", "upf-5"},
        {"dnnId", "internet"},
    };
    EXPECT_TRUE(chf::charging_scope_matches(slice_and_right_upf, request_attributes()));
}

TEST(ChargingScope, AConstrainedAttributeTheRequestDoesNotCarryDoesNotMatch) {
    // Failing closed matters here: matching an offering scoped to something the request never
    // stated would charge a subscriber against a product that does not apply to them.
    EXPECT_FALSE(chf::charging_scope_matches(json{{"ratType", "NR"}}, request_attributes()));
}

TEST(ChargingScope, RoamingIsExpressibleAsAScopeOverTheTwoPlmnIds) {
    // C5's roaming rating falls out of the same mechanism rather than being a separate feature:
    // a home-network scope matches this non-roaming request; a visited-network scope does not.
    const json home = {{"servingCNPlmnId", json{{"mcc", "999"}, {"mnc", "70"}}}};
    EXPECT_TRUE(chf::charging_scope_matches(home, request_attributes()));

    const json visited = {{"servingCNPlmnId", json{{"mcc", "262"}, {"mnc", "01"}}}};
    EXPECT_FALSE(chf::charging_scope_matches(visited, request_attributes()))
        << "a roaming-priced offering must not apply to home traffic";
}

// --- roaming as a first-class scope (ADR-0305, C5) ---
//
// C5 asked that rating distinguish roaming from home traffic. With ADR-0303's mechanism that is a
// scope over a derived `roaming` flag rather than a separate feature -- and deriving it, instead
// of asking operators to scope on `servingCNPlmnId`, is what makes a roaming tariff ONE offering
// instead of one per partner network.

TEST(ChargingScope, ARoamingTariffAppliesOnlyWhenRoaming) {
    json home = request_attributes();
    home["roaming"] = false;
    json visited = request_attributes();
    visited["roaming"] = true;
    visited["servingCNPlmnId"] = json{{"mcc", "262"}, {"mnc", "01"}};

    const json roaming_tariff = {{"roaming", true}};
    EXPECT_FALSE(chf::charging_scope_matches(roaming_tariff, home))
        << "a roaming tariff must not be applied to home traffic";
    EXPECT_TRUE(chf::charging_scope_matches(roaming_tariff, visited));

    const json home_tariff = {{"roaming", false}};
    EXPECT_TRUE(chf::charging_scope_matches(home_tariff, home));
    EXPECT_FALSE(chf::charging_scope_matches(home_tariff, visited))
        << "a home tariff must not silently cover roaming traffic, which is the expensive mistake";
}

TEST(ChargingScope, ARoamingScopeDoesNotMatchARequestThatCannotAnswerTheQuestion) {
    // `roaming` is omitted when the request carries no hPlmnId/servingCNPlmnId pair. Failing
    // closed here matters: defaulting to false would rate genuinely-roaming traffic at the home
    // price whenever a consumer omitted a PLMN field.
    json unknown = request_attributes();
    unknown.erase("roaming");
    EXPECT_FALSE(chf::charging_scope_matches(json{{"roaming", true}}, unknown));
    EXPECT_FALSE(chf::charging_scope_matches(json{{"roaming", false}}, unknown));
}

TEST(ChargingScope, ARoamingDataBundleCombinesRoamingWithASlice) {
    // The composability point: roaming is just another key, so "roaming traffic on slice 1" is one
    // scope rather than a special case in the engine.
    json visited = request_attributes();
    visited["roaming"] = true;
    const json roaming_on_slice_1 = {
        {"roaming", true},
        {"sNSSAI", json{{"sst", 1}, {"sd", "000001"}}},
    };
    EXPECT_TRUE(chf::charging_scope_matches(roaming_on_slice_1, visited));

    json visited_other_slice = visited;
    visited_other_slice["sNSSAI"] = json{{"sst", 10}, {"sd", "00000a"}};
    EXPECT_FALSE(chf::charging_scope_matches(roaming_on_slice_1, visited_other_slice));
}
