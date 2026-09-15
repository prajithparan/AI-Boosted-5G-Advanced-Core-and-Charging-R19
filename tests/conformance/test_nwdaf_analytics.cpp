// NWDAF AnLF rules (ADR-0358) against known feature rows and known NF profiles. No I/O.

#include <chrono>

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
