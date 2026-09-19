// NWDAF AnLF rules (ADR-0358) against known feature rows and known NF profiles. No I/O.

#include <chrono>
#include <optional>

#include "analytics.hpp"

#include <gtest/gtest.h>

namespace {

nwdaf::FeatureRow
row(const std::string& supi, double avg, double stddev, double max, std::int64_t sessions) {
    nwdaf::FeatureRow r;
    r.feature_date = "2026-09-13";
    r.subscriber_identifier = supi;
    r.avg_session_octets = avg;
    r.stddev_session_octets = stddev;
    r.max_session_octets = max;
    r.session_count = sessions;
    return r;
}

nwdaf::AbnormalBehaviourThresholds thresholds() {
    nwdaf::AbnormalBehaviourThresholds t;
    t.sigma_threshold = 3.0;
    t.large_rate_floor_octets = 1000;
    t.session_count_threshold = 50;
    t.max_exception_level = 10;
    t.confidence_full_at_sessions = 30;
    return t;
}

TEST(NwdafAbnormalBehaviour, LargeRateFlowIsJudgedAgainstTheSubscribersOwnDistribution) {
    // A heavy line is not abnormal for being heavy (ADR-0340). Two subscribers, both with a
    // 1 GB max session: for the first that is 6 sigma above its own mean, for the second it is
    // its ordinary day.
    const std::vector<nwdaf::FeatureRow> today = {
        row("imsi-1", 1e6, 1.5e8, 1e9, 40), // (1e9 - 1e6) / 1.5e8 ~ 6.66 sigma -> abnormal
        row("imsi-2", 9e8, 1e8, 1e9, 40),   // 1 sigma -> normal
    };
    const auto out = nwdaf::detect_abnormal_behaviour(today, {}, thresholds());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].excep.excepId.value, sbi_gen::ExceptionId::UNEXPECTED_LARGE_RATE_FLOW);
    ASSERT_TRUE(out[0].supis.has_value());
    ASSERT_EQ(out[0].supis->size(), 1u);
    EXPECT_EQ((*out[0].supis)[0], "imsi-1");
    // Level = whole sigmas above the mean, capped: floor(6.66) = 6.
    ASSERT_TRUE(out[0].excep.excepLevel.has_value());
    EXPECT_EQ(*out[0].excep.excepLevel, 6);
    // Ratio = affected / population, as a percentage: 1 of 2.
    ASSERT_TRUE(out[0].ratio.has_value());
    EXPECT_EQ(*out[0].ratio, 50);
    // Confidence from sample size: 40 sessions against full-at-30 -> capped at 100.
    EXPECT_EQ(*out[0].confidence, 100);
    // No yesterday -> no trend, rather than a fabricated STABLE.
    EXPECT_FALSE(out[0].excep.excepTrend.has_value());
}

TEST(NwdafAbnormalBehaviour, TheAbsoluteFloorStopsNoiseOnTinySessions) {
    // 10 sigma above a mean of 10 bytes is still 100 bytes. Not a large-rate flow.
    const auto out =
        nwdaf::detect_abnormal_behaviour({row("imsi-3", 10, 9, 100, 40)}, {}, thresholds());
    EXPECT_TRUE(out.empty());
}

TEST(NwdafAbnormalBehaviour, TooFrequentServiceAccessAndLevelIsAMultipleOfTheThreshold) {
    const auto out =
        nwdaf::detect_abnormal_behaviour({row("imsi-4", 1e6, 1e5, 1.1e6, 175)}, {}, thresholds());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].excep.excepId.value, sbi_gen::ExceptionId::TOO_FREQUENT_SERVICE_ACCESS);
    EXPECT_EQ(*out[0].excep.excepLevel, 3); // 175 / 50 = 3.5 -> 3
    EXPECT_EQ(*out[0].ratio, 100);
}

TEST(NwdafAbnormalBehaviour, TrendComesOnlyFromTwoRealDays) {
    const std::vector<nwdaf::FeatureRow> yesterday = {row("imsi-5", 1e6, 1e8, 5e8, 40)}; // ~5 sigma
    const std::vector<nwdaf::FeatureRow> today = {row("imsi-5", 1e6, 1e8, 9e8, 40)};     // ~9 sigma
    const auto out = nwdaf::detect_abnormal_behaviour(today, yesterday, thresholds());
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].excep.excepTrend.has_value());
    EXPECT_EQ(out[0].excep.excepTrend->value, sbi_gen::ExceptionTrend::UP);
}

TEST(NwdafAbnormalBehaviour, ZeroStddevNeverDividesAndNeverTrips) {
    // A subscriber with one session has no spread. Not abnormal, and not a division by zero.
    const auto out =
        nwdaf::detect_abnormal_behaviour({row("imsi-6", 5e8, 0, 5e8, 1)}, {}, thresholds());
    EXPECT_TRUE(out.empty());
}

TEST(NwdafNfLoad, CarriesExactlyWhatTheProfileCarried) {
    // TS 23.288 Table 6.5.2-1: NF load and status come from the NRF profile. A profile without
    // `load` yields no load level -- absent, not zero, not estimated.
    sbi_gen::NFProfile_Nnrf_NFManagement with_load;
    with_load.nfInstanceId = "11111111-1111-4111-8111-111111111111";
    with_load.nfType.value = "AMF";
    with_load.nfStatus.value = "REGISTERED";
    with_load.load = 42;
    sbi_gen::NFProfile_Nnrf_NFManagement without_load = with_load;
    without_load.nfInstanceId = "22222222-2222-4222-8222-222222222222";
    without_load.load.reset();

    const auto out = nwdaf::nf_load_from_profiles({with_load, without_load});
    ASSERT_EQ(out.size(), 2u);
    ASSERT_TRUE(out[0].nfLoadLevelAverage.has_value());
    EXPECT_EQ(*out[0].nfLoadLevelAverage, 42);
    EXPECT_EQ(out[0].nfType->value, "AMF");
    // NfStatus is percentage-of-time per status (TS 29.520), not the enum: one observation of a
    // REGISTERED profile is 100% registered, the other buckets absent.
    ASSERT_TRUE(out[0].nfStatus.has_value());
    EXPECT_EQ(out[0].nfStatus->statusRegistered.value_or(-1), 100);
    EXPECT_FALSE(out[0].nfStatus->statusUnregistered.has_value());
    EXPECT_FALSE(out[1].nfLoadLevelAverage.has_value());
    // Statistics carry no confidence (Table 6.5.3-1) -- only ADR-0369's predictions do.
    EXPECT_FALSE(out[0].confidence.has_value());
    EXPECT_FALSE(out[1].confidence.has_value());
}

TEST(NwdafNfLoad, StatusSharesAreTimeWeightedOverTheObservationWindow) {
    // Phase C (ADR-0368): TS 29.520 NfStatus is "the percentage of time spent on various NF
    // states". An instance observed REGISTERED at t0, DEREGISTERED at t0+30s, over a window that
    // ends at t0+40s: 75% registered, 25% unregistered -- and it is reported even though it is no
    // longer in the NRF snapshot. Loads are averaged (and peaked) over what was observed.
    using namespace std::chrono_literals;
    const auto t0 = std::chrono::system_clock::time_point(std::chrono::seconds(1'000'000));
    nwdaf::NfStatusObservation a{"33333333-3333-4333-8333-333333333333",
                                 std::string("NSACF"),
                                 std::nullopt,
                                 t0,
                                 "REGISTERED",
                                 20};
    nwdaf::NfStatusObservation b = a;
    b.at = t0 + 10s;
    b.load = 40;
    nwdaf::NfStatusObservation c = a;
    c.at = t0 + 30s;
    c.status = "DEREGISTERED";
    c.load.reset();
    // A snapshot-only instance keeps Phase A's single observation.
    sbi_gen::NFProfile_Nnrf_NFManagement snap;
    snap.nfInstanceId = "44444444-4444-4444-8444-444444444444";
    snap.nfType.value = "AMF";
    snap.nfStatus.value = "REGISTERED";

    const auto out = nwdaf::nf_load({snap}, {a, b, c}, t0 - 1h, t0 + 40s);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].nfInstanceId.value_or(""), snap.nfInstanceId);
    EXPECT_EQ(out[0].nfStatus->statusRegistered.value_or(-1), 100);
    const auto& observed = out[1];
    EXPECT_EQ(observed.nfInstanceId.value_or(""), a.nf_instance_id);
    EXPECT_EQ(observed.nfType->value, "NSACF");
    ASSERT_TRUE(observed.nfStatus.has_value());
    EXPECT_EQ(observed.nfStatus->statusRegistered.value_or(-1), 75);
    EXPECT_EQ(observed.nfStatus->statusUnregistered.value_or(-1), 25);
    EXPECT_FALSE(observed.nfStatus->statusUndiscoverable.has_value());
    EXPECT_EQ(observed.nfLoadLevelAverage.value_or(-1), 30);
    EXPECT_EQ(observed.nfLoadLevelpeak.value_or(-1), 40);
}

TEST(NwdafNfLoad, ObservationsOutsideTheWindowDoNotCount) {
    using namespace std::chrono_literals;
    const auto t0 = std::chrono::system_clock::time_point(std::chrono::seconds(2'000'000));
    nwdaf::NfStatusObservation old{"55555555-5555-4555-8555-555555555555",
                                   std::string("SMF"),
                                   std::nullopt,
                                   t0 - 2h,
                                   "REGISTERED",
                                   std::nullopt};
    const auto out = nwdaf::nf_load({}, {old}, t0 - 1h, t0);
    EXPECT_TRUE(out.empty()); // no snapshot, no in-window history: nothing to report
}

} // namespace

// ---- ADR-0369: the NF_LOAD model's input contract, on the inference side --------------------

#include "model_runtime.hpp"

TEST(NwdafNfLoadModel, FeatureVectorMatchesTheSidecarsWindowRule) {
    // train_nf_load.py: [lag3, lag2, lag1, lag0, mean of the four, registered share].
    std::vector<nwdaf::LoadSample> h{{10, true}, {20, true}, {30, false}, {40, true}, {50, true}};
    const auto f = nwdaf::nf_load_features(h);
    ASSERT_TRUE(f.has_value());
    EXPECT_FLOAT_EQ((*f)[0], 20.0F);
    EXPECT_FLOAT_EQ((*f)[1], 30.0F);
    EXPECT_FLOAT_EQ((*f)[2], 40.0F);
    EXPECT_FLOAT_EQ((*f)[3], 50.0F);
    EXPECT_FLOAT_EQ((*f)[4], 35.0F);
    EXPECT_FLOAT_EQ((*f)[5], 0.75F);
    EXPECT_STREQ(nwdaf::kNfLoadFeatureNames[0], "load_lag3");
    EXPECT_STREQ(nwdaf::kNfLoadFeatureNames[5], "registered_share");
}

TEST(NwdafNfLoadModel, TooLittleOrLoadlessHistoryYieldsNoFeatures) {
    EXPECT_FALSE(nwdaf::nf_load_features({{10, true}, {20, true}, {30, true}}).has_value());
    // A DEREGISTERED observation carries no load and breaks the window (no padding, no guess).
    EXPECT_FALSE(
        nwdaf::nf_load_features({{10, true}, {std::nullopt, false}, {30, true}, {40, true}})
            .has_value());
}

TEST(NwdafNfLoadModel, RuntimeRejectsBytesThatAreNotAModelAndPredictsNothing) {
    nwdaf::ModelRuntime rt;
    EXPECT_FALSE(rt.load("definitely not ONNX", 7));
    EXPECT_FALSE(rt.loaded());
    EXPECT_FALSE(rt.predict(nwdaf::NfLoadFeatures{}).has_value());
}

// TS 23.288 6.4: SERVICE_EXPERIENCE aggregation of collected SMF QOS_MON reports into a per-slice
// svcExprc. Verifies the generic S-NSSAI rule: standard and custom slices grouped/echoed verbatim.
namespace {
nlohmann::json qos_report(const nlohmann::json& snssai, const std::string& supi, int rt_ms) {
    return nlohmann::json{{"event", "QOS_MON"},
                          {"snssai", snssai},
                          {"supi", supi},
                          {"rtDelays", nlohmann::json::array({rt_ms})}};
}
} // namespace

TEST(NwdafServiceExperience, AggregatesPerSliceStandardAndCustomVerbatim) {
    const nlohmann::json embb{{"sst", 1}};
    const nlohmann::json custom{{"sst", 200}, {"sd", "0a1b2c"}};
    const std::vector<nlohmann::json> reports{qos_report(embb, "imsi-1", 10),
                                              qos_report(embb, "imsi-2", 20),
                                              qos_report(custom, "imsi-3", 90)};

    const auto out = nwdaf::service_experience(reports, std::nullopt);
    ASSERT_EQ(out.size(), 2u);

    // Find each slice by its verbatim snssai.
    auto find = [&](const nlohmann::json& sn) {
        for (const auto& info : out) {
            if (info.snssai && nlohmann::json(*info.snssai) == sn) {
                return info;
            }
        }
        ADD_FAILURE() << "slice not found: " << sn.dump();
        return out.front();
    };
    const auto embb_info = find(embb);
    const auto custom_info = find(custom);

    // The custom slice (SST 200 + SD) survived verbatim.
    ASSERT_TRUE(custom_info.snssai.has_value());
    EXPECT_EQ(nlohmann::json(*custom_info.snssai).at("sst"), 200);
    EXPECT_EQ(nlohmann::json(*custom_info.snssai).at("sd"), "0a1b2c");

    // Lower mean latency (eMBB: mean 15ms) => higher MOS than the custom slice (90ms).
    ASSERT_TRUE(embb_info.svcExprc.mos.has_value());
    ASSERT_TRUE(custom_info.svcExprc.mos.has_value());
    EXPECT_GT(*embb_info.svcExprc.mos, *custom_info.svcExprc.mos);
    EXPECT_GE(*embb_info.svcExprc.mos, 1.0);
    EXPECT_LE(*embb_info.svcExprc.mos, 5.0);
    // Two SUPIs on eMBB, one on the custom slice.
    ASSERT_TRUE(embb_info.supis.has_value());
    EXPECT_EQ(embb_info.supis->size(), 2u);
}

TEST(NwdafServiceExperience, FilterMatchesTheCustomSliceOnlyAndEmptyIsEmpty) {
    const nlohmann::json embb{{"sst", 1}};
    const nlohmann::json custom{{"sst", 200}, {"sd", "0a1b2c"}};
    const std::vector<nlohmann::json> reports{qos_report(embb, "imsi-1", 10),
                                              qos_report(custom, "imsi-2", 40)};

    const auto only_custom =
        nwdaf::service_experience(reports, std::optional<nlohmann::json>(custom));
    ASSERT_EQ(only_custom.size(), 1u);
    EXPECT_EQ(nlohmann::json(*only_custom.front().snssai), custom);

    EXPECT_TRUE(nwdaf::service_experience({}, std::nullopt).empty());
}
