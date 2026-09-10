#include "analytics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mcp {
namespace {

constexpr std::size_t kMinForecastSamples = 2;
constexpr std::size_t kMinBaselineSamples = 3;

} // namespace

ExhaustionForecast forecast_exhaustion(const std::vector<UsagePoint>& points,
                                       double remaining_octets) {
    ExhaustionForecast out;
    out.remaining_octets = remaining_octets;
    out.samples = points.size();

    if (points.size() < kMinForecastSamples) {
        out.reason = "not enough usage history to establish a rate";
        return out;
    }

    const auto [min_it, max_it] =
        std::minmax_element(points.begin(), points.end(), [](const auto& a, const auto& b) {
            return a.at_unix_sec < b.at_unix_sec;
        });
    const std::int64_t span = max_it->at_unix_sec - min_it->at_unix_sec;
    out.observed_span_sec = span;
    if (span <= 0) {
        // Every record carries the same timestamp, so there is no elapsed time to divide by.
        // Returning "0 hours remaining" here would be a fabricated emergency.
        out.reason = "usage records span no elapsed time, so no rate can be derived";
        return out;
    }

    const double total =
        std::accumulate(points.begin(), points.end(), 0.0, [](double acc, const auto& p) {
            return acc + p.used_octets;
        });
    if (total <= 0.0) {
        out.reason = "no measured usage in this window";
        return out;
    }

    out.burn_octets_per_hour = total / (static_cast<double>(span) / 3600.0);
    if (out.burn_octets_per_hour <= 0.0) {
        out.reason = "derived burn rate is not positive";
        return out;
    }
    if (remaining_octets <= 0.0) {
        // A real, useful answer rather than an error: the allowance is already gone.
        out.computable = true;
        out.hours_remaining = 0.0;
        out.reason = "allowance already exhausted";
        return out;
    }

    out.hours_remaining = remaining_octets / out.burn_octets_per_hour;
    out.computable = true;
    return out;
}

SpendAnomaly detect_spend_anomaly(const std::vector<UsagePoint>& points, double z_threshold) {
    SpendAnomaly out;
    // The most recent point is the window under test; everything older is the baseline. A
    // baseline that included the point being tested would drag the mean toward it and hide
    // exactly the spike this is looking for.
    if (points.size() < kMinBaselineSamples + 1) {
        out.reason = "not enough history for a per-subscriber baseline";
        return out;
    }

    std::vector<UsagePoint> sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.at_unix_sec > b.at_unix_sec;
    });

    out.recent_spend = sorted.front().spend;
    const std::vector<UsagePoint> baseline(sorted.begin() + 1, sorted.end());
    out.baseline_samples = baseline.size();

    const double sum =
        std::accumulate(baseline.begin(), baseline.end(), 0.0, [](double acc, const auto& p) {
            return acc + p.spend;
        });
    out.baseline_mean = sum / static_cast<double>(baseline.size());

    double variance = 0.0;
    for (const auto& p : baseline) {
        const double d = p.spend - out.baseline_mean;
        variance += d * d;
    }
    out.baseline_stddev = std::sqrt(variance / static_cast<double>(baseline.size()));

    if (out.baseline_stddev <= 0.0) {
        // A perfectly flat baseline makes a z-score undefined (division by zero). Any deviation
        // at all is then noteworthy, but reporting an infinite z-score would be nonsense, so the
        // comparison is made directly and the absence of a score is stated.
        out.computable = true;
        out.is_anomalous = out.recent_spend > out.baseline_mean;
        out.reason = out.is_anomalous
                         ? "baseline is perfectly flat; any increase is a departure from it"
                         : "baseline is perfectly flat and this window matches it";
        return out;
    }

    out.z_score = (out.recent_spend - out.baseline_mean) / out.baseline_stddev;
    // One-sided on purpose: spending far LESS than usual is not a bill-shock event, and flagging
    // it would train an operator to ignore the alert.
    out.is_anomalous = out.z_score >= z_threshold;
    out.computable = true;
    return out;
}

} // namespace mcp
