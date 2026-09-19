#include "analytics.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

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

// ---- service experience (TS 23.288 6.4) helpers ----
std::optional<double> first_delay(const nlohmann::json& report, const char* key) {
    if (report.contains(key) && report.at(key).is_array() && !report.at(key).empty()) {
        return report.at(key)[0].get<double>();
    }
    return std::nullopt;
}

double round_trip_ms(const nlohmann::json& report) {
    if (const auto rt = first_delay(report, "rtDelays")) {
        return *rt;
    }
    return first_delay(report, "ulDelays").value_or(0.0) +
           first_delay(report, "dlDelays").value_or(0.0);
}

bool has_latency(const nlohmann::json& report) {
    return report.contains("rtDelays") || report.contains("ulDelays") ||
           report.contains("dlDelays");
}

// Disclosed lab mapping: mean round-trip latency (ms) -> MOS in [1.0, 5.0], lower latency giving a
// higher experience (mos = 5 - meanRt/30, clamped). A transparent monotonic derivation, not a
// calibrated model; a conformant MOS comes from the AF's application-layer measurement or a
// trained MTLF model -- the enhancement path.
double latency_to_mos(double mean_rt_ms) {
    return std::clamp(5.0 - mean_rt_ms / 30.0, 1.0, 5.0);
}

} // namespace

std::vector<sbi_gen::ServiceExperienceInfo_Nnwdaf_EventsSubscription>
service_experience(const std::vector<nlohmann::json>& qos_mon_reports,
                   const std::optional<nlohmann::json>& snssai_filter) {
    struct Aggregate {
        nlohmann::json snssai;
        std::vector<double> round_trips;
        std::set<std::string> supis;
    };
    // Keyed on the serialised full S-NSSAI, so a standardised SST (1/2/3) and an operator-specific
    // one (128-255) with any SD are distinct opaque groups -- never coerced or dropped.
    std::map<std::string, Aggregate> by_slice;
    for (const auto& report : qos_mon_reports) {
        if (!report.is_object() || !report.contains("snssai") || !has_latency(report)) {
            continue;
        }
        const nlohmann::json& snssai = report.at("snssai");
        if (snssai_filter && *snssai_filter != snssai) {
            continue; // verbatim filter, any SST/SD
        }
        Aggregate& aggregate = by_slice[snssai.dump()];
        aggregate.snssai = snssai;
        aggregate.round_trips.push_back(round_trip_ms(report));
        if (report.contains("supi") && report.at("supi").is_string()) {
            aggregate.supis.insert(report.at("supi").get<std::string>());
        }
    }

    std::vector<sbi_gen::ServiceExperienceInfo_Nnwdaf_EventsSubscription> out;
    for (auto& [key, aggregate] : by_slice) {
        const double n = static_cast<double>(aggregate.round_trips.size());
        double sum = 0.0;
        for (const double v : aggregate.round_trips) {
            sum += v;
        }
        const double mean = sum / n;
        double variance = 0.0;
        for (const double v : aggregate.round_trips) {
            variance += (v - mean) * (v - mean);
        }
        variance /= n;

        sbi_gen::ServiceExperienceInfo_Nnwdaf_EventsSubscription info;
        info.svcExprc.mos = latency_to_mos(mean);
        info.svcExprcVariance = variance;
        info.snssai = aggregate.snssai.get<sbi_gen::Snssai>(); // echoed verbatim
        info.confidence = static_cast<std::int64_t>(std::min(100.0, n / 30.0 * 100.0));
        if (!aggregate.supis.empty()) {
            info.supis = std::vector<sbi_gen::Supi>(aggregate.supis.begin(), aggregate.supis.end());
        }
        out.push_back(std::move(info));
    }
    return out;
}

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
            // No confidence: statistics (TS 23.288 Table 6.5.3-1) carry none; only predictions
            // (Table 6.5.3-2, ADR-0369) do.
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<sbi_gen::NfLoadLevelInformation>
nf_load(const std::vector<sbi_gen::NFProfile_Nnrf_NFManagement>& snapshot,
        const std::vector<NfStatusObservation>& observations,
        std::chrono::system_clock::time_point window_start,
        std::chrono::system_clock::time_point now) {
    // Group the observations per instance, in time order.
    std::map<std::string, std::vector<const NfStatusObservation*>> history;
    for (const auto& o : observations) {
        if (o.at >= window_start && o.at <= now) {
            history[o.nf_instance_id].push_back(&o);
        }
    }
    for (auto& [id, obs] : history) {
        std::sort(
            obs.begin(), obs.end(), [](const auto* a, const auto* b) { return a->at < b->at; });
    }

    std::vector<sbi_gen::NfLoadLevelInformation> out;
    std::set<std::string> done;
    const auto emit = [&](const std::string& id, const sbi_gen::NFProfile_Nnrf_NFManagement* snap) {
        sbi_gen::NfLoadLevelInformation info;
        info.nfInstanceId = id;
        const auto it = history.find(id);
        if (snap) {
            info.nfType = snap->nfType;
            if (snap->nfSetIdList && !snap->nfSetIdList->empty()) {
                info.nfSetId = snap->nfSetIdList->front();
            }
        }
        if (it == history.end()) {
            if (snap == nullptr) {
                return;
            }
            // No history: the snapshot's single observation, exactly as Phase A reported it.
            sbi_gen::NfStatus status;
            if (snap->nfStatus.value == sbi_gen::NFStatus::REGISTERED) {
                status.statusRegistered = 100;
            } else if (snap->nfStatus.value == sbi_gen::NFStatus::UNDISCOVERABLE) {
                status.statusUndiscoverable = 100;
            } else {
                status.statusUnregistered = 100;
            }
            info.nfStatus = status;
            if (snap->load) {
                info.nfLoadLevelAverage = *snap->load;
                info.nfLoadLevelpeak = *snap->load;
            }
            out.push_back(std::move(info));
            return;
        }
        const auto& obs = it->second;
        // Time-weighted shares: the state at any instant is the latest observation before it.
        using ms = std::chrono::milliseconds;
        double registered = 0, unregistered = 0, undiscoverable = 0;
        for (std::size_t i = 0; i < obs.size(); ++i) {
            const auto from = obs[i]->at;
            const auto to = i + 1 < obs.size() ? obs[i + 1]->at : now;
            const double d = static_cast<double>(std::chrono::duration_cast<ms>(to - from).count());
            if (obs[i]->status == "REGISTERED") {
                registered += d;
            } else if (obs[i]->status == "UNDISCOVERABLE") {
                undiscoverable += d;
            } else {
                unregistered += d;
            }
            if (obs[i]->nf_type && !info.nfType) {
                sbi_gen::NFType t;
                t.value = *obs[i]->nf_type;
                info.nfType = t;
            }
            if (obs[i]->nf_set_id && !info.nfSetId) {
                info.nfSetId = *obs[i]->nf_set_id;
            }
        }
        const double total = registered + unregistered + undiscoverable;
        sbi_gen::NfStatus status;
        const auto share = [&](double part) -> std::optional<sbi_gen::SamplingRatio> {
            if (total <= 0) {
                return std::nullopt;
            }
            const auto pct = static_cast<std::int64_t>(std::lround(100.0 * part / total));
            if (pct <= 0) {
                return std::nullopt; // SamplingRatio is 1..100; a zero share is absent
            }
            return pct;
        };
        if (total <= 0) {
            // One observation, this instant: 100% in the state it reported.
            const auto& s = obs.back()->status;
            if (s == "REGISTERED") {
                status.statusRegistered = 100;
            } else if (s == "UNDISCOVERABLE") {
                status.statusUndiscoverable = 100;
            } else {
                status.statusUnregistered = 100;
            }
        } else {
            status.statusRegistered = share(registered);
            status.statusUnregistered = share(unregistered);
            status.statusUndiscoverable = share(undiscoverable);
        }
        info.nfStatus = status;
        // Load over the window: mean and peak of what the profiles reported.
        std::int64_t sum = 0, peak = 0, n = 0;
        for (const auto* o : obs) {
            if (o->load) {
                sum += *o->load;
                peak = std::max(peak, *o->load);
                ++n;
            }
        }
        if (n > 0) {
            info.nfLoadLevelAverage = sum / n;
            info.nfLoadLevelpeak = peak;
        } else if (snap && snap->load) {
            info.nfLoadLevelAverage = *snap->load;
            info.nfLoadLevelpeak = *snap->load;
        }
        // Statistics carry no confidence (TS 23.288 Table 6.5.3-1); predictions do (ADR-0369).
        out.push_back(std::move(info));
    };
    for (const auto& p : snapshot) {
        done.insert(p.nfInstanceId);
        emit(p.nfInstanceId, &p);
    }
    for (const auto& [id, obs] : history) {
        if (!done.count(id)) {
            emit(id, nullptr);
        }
    }
    return out;
}

} // namespace nwdaf
