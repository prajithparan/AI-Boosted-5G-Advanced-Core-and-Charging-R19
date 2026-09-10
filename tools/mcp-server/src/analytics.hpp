#pragma once

// ADR-0334: quota-exhaustion forecasting and spend-anomaly detection.
//
// Both are computed from the subscriber's OWN recorded CDRs. Neither is a trained model, and the
// distinction is deliberate rather than a shortcut:
//
//   * Time-to-exhaustion is `remaining / burn_rate`. That is arithmetic. Wrapping a division in a
//     regressor would add opacity without adding accuracy, and would make a number that can be
//     checked by hand into one that cannot.
//   * A spend anomaly is "this window departs from this subscriber's own history by more than N
//     standard deviations". A per-subscriber baseline needs no global training set, and it cannot
//     encode a population bias that a shared model can.
//
// Where a MODEL genuinely helps is predicting the FUTURE rate rather than extrapolating the
// observed one -- which is exactly what CHF's existing quota-sizing regressor already does
// (ADR-0074). Wiring its prediction in as an alternative basis is the natural next step, and is
// named here rather than half-built: it needs the model trained on real CDRs rather than the
// synthetic bootstrap its own training script discloses it may fall back to.
//
// Every result carries the basis it was computed from and the number of records behind it, so a
// thin projection is visibly thin instead of arriving with the same confidence as a solid one.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

struct UsagePoint {
    std::int64_t at_unix_sec = 0;
    double used_octets = 0.0;
    double spend = 0.0;
};

struct ExhaustionForecast {
    bool computable = false;
    // Why not, when it is not. An agent must be able to say "not enough history" rather than
    // present a guess as a forecast.
    std::string reason;
    double burn_octets_per_hour = 0.0;
    double remaining_octets = 0.0;
    double hours_remaining = 0.0;
    std::size_t samples = 0;
    std::int64_t observed_span_sec = 0;
};

// Projects from the observed burn rate over the supplied points. Returns computable=false when
// there is too little history or no elapsed time to divide by -- never a zero or infinite
// "forecast", both of which read as facts.
ExhaustionForecast forecast_exhaustion(const std::vector<UsagePoint>& points,
                                       double remaining_octets);

struct SpendAnomaly {
    bool computable = false;
    std::string reason;
    double recent_spend = 0.0;
    double baseline_mean = 0.0;
    double baseline_stddev = 0.0;
    double z_score = 0.0;
    bool is_anomalous = false;
    std::size_t baseline_samples = 0;
};

// Compares the most recent window's spend against the subscriber's own earlier windows.
// `z_threshold` is the operator's sensitivity choice, not this code's: what counts as a
// bill-shock alert is a commercial decision.
SpendAnomaly detect_spend_anomaly(const std::vector<UsagePoint>& points, double z_threshold);

} // namespace mcp
