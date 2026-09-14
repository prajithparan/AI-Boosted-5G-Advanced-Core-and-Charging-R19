// Round-trip conformance tests for LMF's Nlmf_Broadcast + Nlmf_DataExposure generated DTOs
// (docs/DECISIONS.md ADR-0196, gap-closure per ADR-0193). Source:
// specs/5G_APIs-REL-19/TS29572_Nlmf_Broadcast.yaml, TS29572_Nlmf_DataExposure.yaml. Same
// precedent as test_lmf_dtos.cpp: construct -> to_json -> from_json -> compare, using the real
// generated types, not synthetic ones. Both land in their own standalone headers (no cross-file
// $ref cycle pulled them into the shared TS26510_CommonData_grp.hpp).

#include <nlohmann/json.hpp>

#include "TS26510_CommonData_grp.hpp"
#include "TS26510_CommonData_grp.hpp" // was TS29572_Nlmf_DataExposure.hpp; merged into the group by ADR-0358's regeneration
#include "TS29572_Nlmf_Broadcast.hpp"

#include <gtest/gtest.h>

TEST(LmfBroadcastDtos, CipherRequestDataRoundTrips) {
    sbi_gen::CipherRequestData original;
    original.amfCallBackURI = "https://amf.example/callback";

    nlohmann::json j = original;
    auto decoded = j.get<sbi_gen::CipherRequestData>();
    EXPECT_EQ(decoded.amfCallBackURI, "https://amf.example/callback");
}

TEST(LmfBroadcastDtos, CipherResponseDataRoundTrips) {
    sbi_gen::CipherResponseData original;
    original.dataAvailability = "CIPHERING_KEY_DATA_NOT_AVAILABLE";

    nlohmann::json j = original;
    auto decoded = j.get<sbi_gen::CipherResponseData>();
    EXPECT_EQ(decoded.dataAvailability, "CIPHERING_KEY_DATA_NOT_AVAILABLE");
}

TEST(LmfDataExposureDtos, LmfDataExposureSubscriptionRoundTrips) {
    sbi_gen::LmfDataExposureSubscription original;
    original.notificationUri = "https://consumer.example/notify";
    original.notifyCorrelationId = "corr-1";
    original.aoi.presenceState = sbi_gen::PresenceState{sbi_gen::PresenceState::IN_AREA};

    nlohmann::json j = original;
    auto decoded = j.get<sbi_gen::LmfDataExposureSubscription>();
    EXPECT_EQ(decoded.notificationUri, "https://consumer.example/notify");
    EXPECT_EQ(decoded.notifyCorrelationId, "corr-1");
}
