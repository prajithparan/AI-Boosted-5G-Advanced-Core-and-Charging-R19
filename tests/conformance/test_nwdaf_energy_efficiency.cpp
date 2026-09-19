// NWDAF energy-efficiency analytics (R19 addition; KPI per TS 28.554 6.7.1, over the collected SMF
// QOS_MON ulDataRate/dlDataRate). Verifies the TS 28.554 EE = data-volume / energy-consumption math
// against a hand-computed case, the generic S-NSSAI handling the user required (a standardised SST
// and an operator-specific one both grouped and echoed verbatim), the verbatim per-slice filter,
// and that a report carrying no data rate contributes nothing (no fabricated volume).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

#include "analytics.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;

json qos_report(const json& snssai,
                const std::string& ul,
                const std::string& dl,
                const std::string& supi = "imsi-1") {
    return json{{"event", "QOS_MON"},
                {"snssai", snssai},
                {"supi", supi},
                {"ulDataRate", ul},
                {"dlDataRate", dl}};
}

const nwdaf::SliceEnergyEfficiency* find(const std::vector<nwdaf::SliceEnergyEfficiency>& v,
                                         const json& snssai) {
    for (const auto& e : v) {
        if (e.snssai == snssai) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

TEST(NwdafEnergyEfficiency, KpiMathMatchesTs28554Definition) {
    const json embb{{"sst", 1}};
    // One report: 100 Mbps up + 100 Mbps down = 200 Mbps total = 2e8 bps.
    const std::vector<json> reports{qos_report(embb, "100 Mbps", "100 Mbps")};
    nwdaf::EnergyModel model;
    model.static_watts_per_slice = 100.0; // 100 W baseline
    model.joules_per_gigabyte = 0.0;      // isolate the static term for an exact check
    const double window = 10.0;           // seconds

    const auto out = nwdaf::energy_efficiency(reports, std::nullopt, model, window);
    ASSERT_EQ(out.size(), 1u);
    const auto& ee = out[0];
    // volume = 2e8 bps * 10 s = 2e9 bits; energy = 100 W * 10 s = 1000 J; EE = 2e9 / 1000 = 2e6.
    EXPECT_NEAR(ee.data_volume_bits, 2e9, 1.0);
    EXPECT_NEAR(ee.energy_joules, 1000.0, 1e-6);
    EXPECT_NEAR(ee.efficiency_bit_per_joule, 2e6, 1.0);
    EXPECT_EQ(ee.samples, 1u);
}

TEST(NwdafEnergyEfficiency, GroupsEverySliceVerbatimStandardAndCustom) {
    const json embb{{"sst", 1}};
    const json custom{{"sst", 200}, {"sd", "0a1b2c"}};
    const std::vector<json> reports{
        qos_report(embb, "10 Mbps", "10 Mbps", "imsi-a"),
        qos_report(custom, "50 Mbps", "50 Mbps", "imsi-b"),
        qos_report(custom, "30 Mbps", "30 Mbps", "imsi-c"),
    };
    nwdaf::EnergyModel model;
    model.static_watts_per_slice = 50.0;
    model.joules_per_gigabyte = 1.0;

    const auto out = nwdaf::energy_efficiency(reports, std::nullopt, model, 1.0);
    ASSERT_EQ(out.size(), 2u);
    // Both slices present, keys echoed byte-for-byte (custom SST + SD not lost or coerced).
    const auto* e = find(out, embb);
    const auto* c = find(out, custom);
    ASSERT_NE(e, nullptr);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->snssai, custom);
    EXPECT_EQ(c->samples, 2u); // two reports meaned for the custom slice
    // Custom mean total rate = ((50+50)+(30+30))/2 = 80 Mbps = 8e7 bps.
    EXPECT_NEAR(c->data_volume_bits, 8e7, 1.0);
}

TEST(NwdafEnergyEfficiency, FilterMatchesTheCustomSliceVerbatim) {
    const json embb{{"sst", 1}};
    const json custom{{"sst", 200}, {"sd", "0a1b2c"}};
    const std::vector<json> reports{qos_report(embb, "10 Mbps", "10 Mbps"),
                                    qos_report(custom, "20 Mbps", "20 Mbps")};
    nwdaf::EnergyModel model;
    model.static_watts_per_slice = 1.0;

    const auto out = nwdaf::energy_efficiency(reports, std::optional<json>(custom), model, 1.0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].snssai, custom);
}

TEST(NwdafEnergyEfficiency, ReportWithoutRateContributesNoVolume) {
    const json embb{{"sst", 1}};
    std::vector<json> reports{
        json{{"event", "QOS_MON"}, {"snssai", embb}, {"supi", "imsi-1"}}, // no ul/dl rate
    };
    nwdaf::EnergyModel model;
    model.static_watts_per_slice = 10.0;
    const auto out = nwdaf::energy_efficiency(reports, std::nullopt, model, 1.0);
    EXPECT_TRUE(out.empty()) << "a slice with no rate sample is omitted, not zero-filled";
}
