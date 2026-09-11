#include "profiles.hpp"

#include <array>

namespace cdrgen {
namespace {

// A cheap deterministic hash. Not cryptographic and does not need to be: it only has to spread
// indices evenly and give the same answer everywhere, so that "subscriber 4711" means the same
// customer in every worker, every process and every re-run.
std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

double unit_interval(std::uint64_t h) {
    return static_cast<double>(h % 100000) / 100000.0;
}

} // namespace

const char* to_string(Segment s) {
    return s == Segment::Enterprise ? "enterprise" : "consumer";
}

const char* to_string(ProductKind p) {
    switch (p) {
        case ProductKind::DataBundle:
            return "data_bundle";
        case ProductKind::ServiceUnits:
            return "service_units";
        case ProductKind::TimeBased:
            return "time_based";
        case ProductKind::VoiceAndData:
            return "voice_and_data";
        case ProductKind::SliceScoped:
            return "slice_scoped";
        case ProductKind::Roaming:
            return "roaming";
        case ProductKind::SharedBundle:
            return "shared_bundle";
    }
    return "data_bundle";
}

Profile profile_for(int subscriber_index, int total_subscribers) {
    Profile p;
    const auto h = mix(static_cast<std::uint64_t>(subscriber_index) + 0x9e3779b97f4a7c15ULL);

    // 20% enterprise. Deliberately a minority, as in a real base: a generator that made them half
    // the population would teach a model that enterprise volumes are ordinary.
    p.segment = (h % 100) < 20 ? Segment::Enterprise : Segment::Consumer;

    // Product mix differs by segment, because it does in reality. Consumers mostly buy data;
    // enterprises skew to slice-scoped and shared lines.
    const auto pick = mix(h + 1) % 100;
    if (p.segment == Segment::Enterprise) {
        p.product = pick < 40   ? ProductKind::SliceScoped
                    : pick < 65 ? ProductKind::SharedBundle
                    : pick < 80 ? ProductKind::DataBundle
                    : pick < 90 ? ProductKind::ServiceUnits
                                : ProductKind::Roaming;
    } else {
        p.product = pick < 45   ? ProductKind::DataBundle
                    : pick < 60 ? ProductKind::VoiceAndData
                    : pick < 72 ? ProductKind::TimeBased
                    : pick < 84 ? ProductKind::SharedBundle
                    : pick < 93 ? ProductKind::Roaming
                                : ProductKind::ServiceUnits;
    }

    // Per-subscriber mean volume, the ADR-0340 property. Enterprise sits ~e^2 (about 7x) above
    // consumer, with spread inside each band so neither collapses to a constant.
    const double spread = unit_interval(mix(h + 2));
    p.log_mean_volume =
        p.segment == Segment::Enterprise ? 15.8 + 2.2 * spread : 13.2 + 2.0 * spread;

    // Rating group carries the product: 1 data, 2 voice, 3 events, 4 slice-scoped enterprise.
    switch (p.product) {
        case ProductKind::VoiceAndData:
            p.rating_group = (mix(h + 3) % 2 == 0) ? 1 : 2;
            break;
        case ProductKind::ServiceUnits:
            p.rating_group = 3;
            break;
        case ProductKind::SliceScoped:
            p.rating_group = 4;
            break;
        case ProductKind::TimeBased:
            p.rating_group = 2;
            break;
        default:
            p.rating_group = 1;
            break;
    }

    // Slice: enterprises get dedicated slices, consumers ride the default eMBB slice.
    if (p.segment == Segment::Enterprise) {
        p.slice_sst = 2;
        const auto sd = mix(h + 4) % 3;
        p.slice_sd = sd == 0 ? "000101" : (sd == 1 ? "000102" : "000103");
        p.dnn =
            p.product == ProductKind::SliceScoped ? "enterprise.private" : "enterprise.internet";
        p.upf_id = static_cast<std::uint32_t>(5 + (mix(h + 5) % 3));
        p.charging_characteristics = "0800";
    } else {
        p.slice_sst = 1;
        p.slice_sd = "000001";
        p.dnn = "internet";
        p.upf_id = static_cast<std::uint32_t>(1 + (mix(h + 5) % 3));
        p.charging_characteristics = "0400";
    }

    p.rat_type = (mix(h + 6) % 10 < 8) ? "NR" : "EUTRA";

    // Roaming: the serving PLMN differs from home. Several partner PLMNs so a roaming tariff is
    // exercised across partners rather than one.
    if (p.product == ProductKind::Roaming) {
        static constexpr std::array<const char*, 3> kPartnerMcc{"262", "208", "234"};
        static constexpr std::array<const char*, 3> kPartnerMnc{"01", "10", "15"};
        const auto partner = mix(h + 7) % kPartnerMcc.size();
        p.serving_mcc = kPartnerMcc[partner];
        p.serving_mnc = kPartnerMnc[partner];
    }

    // Shared bundles: members map onto one owner, so several SUPIs genuinely draw down one bucket.
    // Group size 4 -- large enough that sharing is visible in the data, small enough that a group
    // is not the whole population.
    if (p.product == ProductKind::SharedBundle && total_subscribers > 0) {
        const int group = subscriber_index / 4;
        const int owner = (group * 4) % total_subscribers;
        p.shared_bucket = true;
        p.bucket_owner_supi = std::to_string(owner);
    }

    return p;
}

} // namespace cdrgen
