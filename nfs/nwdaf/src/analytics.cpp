#include "analytics.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace nwdaf {
namespace {

std::int64_t clamp_level(double level, std::int64_t max_level) {
    const auto rounded = static_cast<std::int64_t>(std::floor(level));
    return std::clamp<std::int64_t>(rounded, 1, max_level);
}

std::int64_t confidence_for(std::int64_t session_count, std::int64_t full_at) {
    if (full_at <= 0) {
        return 100;
    }
    const auto pct = (100 * session_count) / full_at;
    return std::clamp<std::int64_t>(pct, 0, 100);
}

// One exception across the whole population, listing every subscriber that tripped it -- the
// shape Table 6.7.5.3-1 defines (Exception ID, level, SUPI list, ratio), not one entry per UE.
struct Tripped {
    std::vector<std::string> supis;
    double worst_level = 0;
    std::int64_t min_sessions = 0;
    std::optional<std::string> trend;
};

} // namespace

std::vector<sbi_gen::AbnormalBehaviour>
detect_abnormal_behaviour(const std::vector<FeatureRow>& today,
                          const std::vector<FeatureRow>& yesterday,
                          const AbnormalBehaviourThresholds& t) {
    std::map<std::string, const FeatureRow*> prior;
    for (const auto& r : yesterday) {
        prior[r.subscriber_identifier] = &r;
    }

    Tripped large_rate;
    Tripped frequent;
    // Trend counters: how many affected subscribers were also affected yesterday, worse or better.
    std::int64_t large_rate_up = 0, large_rate_down = 0;

    for (const auto& r : today) {
        // UNEXPECTED_LARGE_RATE_FLOW: this subscriber's own distribution, not a global one -- a
        // heavy enterprise line is not "abnormal" for being heavy (ADR-0340's whole lesson).
        if (r.stddev_session_octets > 0 && r.max_session_octets > t.large_rate_floor_octets) {
            const double sigmas =
                (r.max_session_octets - r.avg_session_octets) / r.stddev_session_octets;
            if (sigmas > t.sigma_threshold) {
                large_rate.supis.push_back(r.subscriber_identifier);
                large_rate.worst_level = std::max(large_rate.worst_level, sigmas);
                large_rate.min_sessions = large_rate.supis.size() == 1
                                              ? r.session_count
                                              : std::min(large_rate.min_sessions, r.session_count);
                if (const auto it = prior.find(r.subscriber_identifier); it != prior.end()) {
                    const auto& y = *it->second;
                    if (y.stddev_session_octets > 0) {
                        const double ysig =
                            (y.max_session_octets - y.avg_session_octets) / y.stddev_session_octets;
                        (sigmas > ysig ? large_rate_up : large_rate_down)++;
                    }
                }
            }
        }
        // TOO_FREQUENT_SERVICE_ACCESS: an absolute daily session ceiling, from config.
        if (t.session_count_threshold > 0 && r.session_count > t.session_count_threshold) {
            frequent.supis.push_back(r.subscriber_identifier);
            frequent.worst_level = std::max(frequent.worst_level,
                                            static_cast<double>(r.session_count) /
                                                static_cast<double>(t.session_count_threshold));
            frequent.min_sessions = frequent.supis.size() == 1
                                        ? r.session_count
                                        : std::min(frequent.min_sessions, r.session_count);
        }
    }

    std::vector<sbi_gen::AbnormalBehaviour> out;
    const double population = static_cast<double>(std::max<std::size_t>(today.size(), 1));

    auto emit = [&](const Tripped& tr, const std::string& id, std::optional<std::string> trend) {
        if (tr.supis.empty()) {
            return;
        }
        sbi_gen::AbnormalBehaviour ab;
        ab.excep.excepId.value = id;
        ab.excep.excepLevel = clamp_level(tr.worst_level, t.max_exception_level);
        if (trend) {
            sbi_gen::ExceptionTrend et;
            et.value = *trend;
            ab.excep.excepTrend = et;
        }
        ab.supis = std::vector<sbi_gen::Supi>();
        for (const auto& s : tr.supis) {
            ab.supis->push_back(s);
        }
        // Ratio: "estimated percentage of UEs affected within the Target of Analytics Reporting".
        sbi_gen::SamplingRatio ratio;
        ratio = static_cast<std::int64_t>(
            std::lround(100.0 * static_cast<double>(tr.supis.size()) / population));
        ab.ratio = ratio;
        ab.confidence = confidence_for(tr.min_sessions, t.confidence_full_at_sessions);
        out.push_back(std::move(ab));
    };

    std::optional<std::string> lr_trend;
    if (large_rate_up + large_rate_down > 0) {
        lr_trend = large_rate_up > large_rate_down   ? sbi_gen::ExceptionTrend::UP
                   : large_rate_down > large_rate_up ? sbi_gen::ExceptionTrend::DOWN
                                                     : sbi_gen::ExceptionTrend::STABLE;
    }
    emit(large_rate, sbi_gen::ExceptionId::UNEXPECTED_LARGE_RATE_FLOW, lr_trend);
    emit(frequent, sbi_gen::ExceptionId::TOO_FREQUENT_SERVICE_ACCESS, std::nullopt);
    return out;
}

std::vector<sbi_gen::NfLoadLevelInformation>
nf_load_from_profiles(const std::vector<sbi_gen::NFProfile_Nnrf_NFManagement>& profiles) {
    std::vector<sbi_gen::NfLoadLevelInformation> out;
    out.reserve(profiles.size());
    for (const auto& p : profiles) {
        sbi_gen::NfLoadLevelInformation info;
        info.nfType = p.nfType;
        info.nfInstanceId = p.nfInstanceId;
        // TS 29.520's NfStatus is NOT the NRF's status enum. Its YAML: "Contains the percentage of
        // time spent on various NF states" -- statusRegistered / statusUnregistered /
        // statusUndiscoverable, each a SamplingRatio. TS 23.288 6.5.3 says the same: availability
        // over the analytics period as a percentage per status. Phase A observes each NF ONCE
        // (one discovery, one moment), so the honest value is 100% in the status the NRF currently
        // holds and nothing in the others. A time-weighted share needs repeated observation, which
        // is DataManagement/Phase C. SUSPENDED is not one of the three buckets the YAML defines;
        // it is reported as not-registered rather than silently dropped.
        sbi_gen::NfStatus status;
        if (p.nfStatus.value == sbi_gen::NFStatus::REGISTERED) {
            status.statusRegistered = 100;
        } else if (p.nfStatus.value == sbi_gen::NFStatus::UNDISCOVERABLE) {
            status.statusUndiscoverable = 100;
        } else {
            status.statusUnregistered = 100;
        }
        info.nfStatus = status;
        if (p.nfSetIdList && !p.nfSetIdList->empty()) {
            info.nfSetId = p.nfSetIdList->front();
        }
        // The profile's own load IS the analytic (Table 6.5.2-1). Absent stays absent.
        if (p.load) {
            info.nfLoadLevelAverage = *p.load;
            info.nfLoadLevelpeak = *p.load; // one sample: average and peak are the same number
            info.confidence = 100;          // reported by the NF itself via NRF, not estimated
        }
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace nwdaf
