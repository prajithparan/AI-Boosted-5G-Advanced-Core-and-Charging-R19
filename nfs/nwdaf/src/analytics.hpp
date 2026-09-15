#pragma once

// NWDAF Analytics Logical Function (AnLF) -- TS 23.288 clause 6, Phase A (ADR-0358).
//
// Pure functions from real inputs to the generated TS 29.520 output types. No I/O here, so every
// rule is unit-testable against known rows, and the data-collection half (feature_store.cpp,
// main.cpp's NRF discovery) can be swapped without touching an analytic.
//
// Two analytics in Phase A, each with its source named by the spec:
//
//   NF_LOAD (6.5). Table 6.5.2-1: "NF load -- Source: NRF -- the load of specific NF instance(s)
//   in their NF profile", "NF status -- Source: NRF". So the input is NFProfile as the NRF
//   returns it, and the output carries exactly the profile's own load/status/capacity. Nothing is
//   estimated: an NF that reported no `load` gets no nfLoadLevelAverage, rather than a guess.
//
//   ABNORMAL_BEHAVIOUR (6.7.5). Table 6.7.5.3-1: a list of Exceptions, each with an Exception ID
//   from the spec's fixed set, an Exception Level ("scalar value indicating the severity"), the
//   affected SUPIs, and a Ratio. The input here is the CHF feature store (ADR-0350): one row per
//   subscriber per day with mean, max and standard deviation of session volume and the session
//   count -- real charging data, not synthetic. Two of the nine spec-defined exception ids are
//   derivable from that data and are implemented:
//
//     UNEXPECTED_LARGE_RATE_FLOW  -- max_session_octets is more than `sigma_threshold` standard
//                                    deviations above the subscriber's OWN mean (and above an
//                                    absolute floor, so a subscriber whose sessions are all tiny
//                                    does not trip on noise).
//     TOO_FREQUENT_SERVICE_ACCESS -- session_count above `session_count_threshold` in one day.
//
//   The other seven (UE location, wakeup, DDoS, wrong destination, radio link failures, ping-pong)
//   need inputs this project does not collect yet -- AMF/SMF events over the bus -- and are NOT
//   produced. An analytic that is not computed is absent from the output, never zero-filled.
//
// What is implementation-defined and how it is defined here, because the spec leaves it open:
//   * excepLevel: the integer number of standard deviations above the mean (large-rate) or the
//     integer multiple of the threshold (frequent-access), capped at `max_exception_level`.
//   * confidence: the spec's Uinteger; here a 0..100 value that grows with the sample size the
//     row was computed from (session_count), reaching 100 at `confidence_full_at_sessions`.
//   * excepTrend needs two consecutive days for the same subscriber; when only one row exists it
//     is omitted rather than reported as STABLE.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "TS26510_CommonData_grp.hpp"

namespace nwdaf {

// The subset of chf_features.subscriber_features an analytic reads.
struct FeatureRow {
    std::string feature_date; // YYYY-MM-DD
    std::string subscriber_identifier;
    std::int64_t session_count = 0;
    double total_used_octets = 0;
    double max_session_octets = 0;
    double avg_session_octets = 0;
    double stddev_session_octets = 0;
    std::int64_t roaming_cdrs = 0;
};

struct AbnormalBehaviourThresholds {
    double sigma_threshold = 3.0;
    double large_rate_floor_octets = 0; // absolute floor, from config
    std::int64_t session_count_threshold = 0;
    std::int64_t max_exception_level = 10;
    std::int64_t confidence_full_at_sessions = 30;
};

// TS 23.288 6.7.5 over the feature store. `today` and, optionally, `yesterday` rows for the same
// population; trend is only computed where a subscriber appears in both.
std::vector<sbi_gen::AbnormalBehaviour>
detect_abnormal_behaviour(const std::vector<FeatureRow>& today,
                          const std::vector<FeatureRow>& yesterday,
                          const AbnormalBehaviourThresholds& thresholds);

// TS 23.288 6.5 over NRF-sourced profiles. One output per profile, carrying only what the profile
// carried. Phase A's single-observation form: every status share is 100% in the current status.
std::vector<sbi_gen::NfLoadLevelInformation>
nf_load_from_profiles(const std::vector<sbi_gen::NFProfile_Nnrf_NFManagement>& profiles);

// Phase C (ADR-0368): one observation of an NF instance, as the NRF's NFStatusNotify delivered it
// through the DCCF / Messaging Framework (TS 29.510 NotificationData): the status the profile
// carried at that moment (or DEREGISTERED), and its load when the profile reported one.
struct NfStatusObservation {
    std::string nf_instance_id;
    std::optional<std::string> nf_type;
    std::optional<std::string> nf_set_id;
    std::chrono::system_clock::time_point at;
    std::string status; // "REGISTERED" | "SUSPENDED" | "UNDISCOVERABLE" | "DEREGISTERED"
    std::optional<std::int64_t> load;
};

// TS 23.288 6.5 over the current NRF snapshot AND the collected observations: TS 29.520's NfStatus
// is "the percentage of time spent on various NF states", so for an instance with a history inside
// [window_start, now] the shares are time-weighted over that history (from its first observation
// or the window start, whichever is later), and nfLoadLevelAverage / nfLoadLevelpeak are the mean
// and maximum of the loads observed. An instance with no history keeps the snapshot's single
// observation (100% in the current status, the profile's own load). An instance that left the NRF
// inside the window is still reported -- its unregistered share is the point of collecting.
std::vector<sbi_gen::NfLoadLevelInformation>
nf_load(const std::vector<sbi_gen::NFProfile_Nnrf_NFManagement>& snapshot,
        const std::vector<NfStatusObservation>& observations,
        std::chrono::system_clock::time_point window_start,
        std::chrono::system_clock::time_point now);

} // namespace nwdaf
