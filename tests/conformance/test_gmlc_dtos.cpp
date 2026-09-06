// Round-trip conformance tests for GMLC's generated DTOs (docs/DECISIONS.md ADR-0189).
// Source: specs/5G_APIs-REL-19/TS29515_Ngmlc_Location.yaml. Same precedent as
// test_smsf_dtos.cpp/test_eir_dtos.cpp: construct -> to_json -> from_json -> compare, using the
// real generated types, not synthetic ones. These types land in the shared,
// strongly-connected-component-grouped TS26510_CommonData_grp.hpp rather than a standalone
// TS29515_Ngmlc_Location.hpp -- see nfs/gmlc/src/main.cpp's own include comment for why.

#include <nlohmann/json.hpp>

#include "TS26510_CommonData_grp.hpp"

#include <gtest/gtest.h>

TEST(GmlcDtos, LocUpdateDataRoundTrips) {
    sbi_gen::LocUpdateData_Ngmlc_Location original;
    original.supi = "imsi-001010000000001";
    original.locationRequestType.value = "MO_LR";
    original.locationEstimate = nlohmann::json{{"point", {{"lat", 1.0}, {"lon", 2.0}}}};
    original.ageOfLocationEstimate = 5;
    original.accuracyFulfilmentIndicator.value = "REQUESTED_ACCURACY_FULFILLED";
    original.lcsQosClass.value = "BEST_EFFORT";

    nlohmann::json j = original;
    auto decoded = j.get<sbi_gen::LocUpdateData_Ngmlc_Location>();
    EXPECT_EQ(decoded.supi, "imsi-001010000000001");
    EXPECT_EQ(decoded.locationRequestType.value, "MO_LR");
    EXPECT_EQ(decoded.ageOfLocationEstimate, 5);
    EXPECT_EQ(decoded.lcsQosClass.value, "BEST_EFFORT");
}

TEST(GmlcDtos, LocUpdateSubsRoundTrips) {
    sbi_gen::LocUpdateSubs original;
    original.nfInstanceId = "5ba9a927-1d31-4c8e-8a10-0000000000cc";
    original.notifURI = "https://consumer.example/notify";

    nlohmann::json j = original;
    auto decoded = j.get<sbi_gen::LocUpdateSubs>();
    EXPECT_EQ(decoded.nfInstanceId, "5ba9a927-1d31-4c8e-8a10-0000000000cc");
    EXPECT_EQ(decoded.notifURI, "https://consumer.example/notify");
}

TEST(GmlcDtos, PrivacyCheckIdMappingRoundTrips) {
    sbi_gen::PrivacyCheckIdMappingRespData original;
    original.gpsiList = std::vector<sbi_gen::Gpsi>{"msisdn-15550100001"};
    original.appLayerIds = std::vector<sbi_gen::ApplicationlayerId>{"applayer-alice"};

    nlohmann::json j = original;
    auto decoded = j.get<sbi_gen::PrivacyCheckIdMappingRespData>();
    ASSERT_TRUE(decoded.gpsiList.has_value());
    ASSERT_TRUE(decoded.appLayerIds.has_value());
    EXPECT_EQ((*decoded.gpsiList)[0], "msisdn-15550100001");
    EXPECT_EQ((*decoded.appLayerIds)[0], "applayer-alice");
}
