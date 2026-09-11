#include "profiles.hpp"

#include <array>
#include <string>

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
        case ProductKind::Sms:
            return "sms";
        case ProductKind::Mms:
            return "mms";
        case ProductKind::VoiceStep:
            return "voice_step";
        case ProductKind::ContentVideo:
            return "content_video";
        case ProductKind::ContentSocial:
            return "content_social";
        case ProductKind::ContentMusic:
            return "content_music";
        case ProductKind::ThrottledTier:
            return "throttled_tier";
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
    // ADR-0342: a full catalogue rather than a handful. Weights are an operator's commercial
    // choice, not a fact -- chosen so every product appears in useful numbers while data stays
    // dominant, as it is in any real base.
    if (p.segment == Segment::Enterprise) {
        p.product = pick < 25   ? ProductKind::SliceScoped
                    : pick < 40 ? ProductKind::SharedBundle
                    : pick < 52 ? ProductKind::DataBundle
                    : pick < 60 ? ProductKind::ServiceUnits
                    : pick < 68 ? ProductKind::Roaming
                    : pick < 76 ? ProductKind::VoiceStep
                    : pick < 82 ? ProductKind::Sms
                    : pick < 87 ? ProductKind::Mms
                    : pick < 92 ? ProductKind::ContentVideo
                    : pick < 96 ? ProductKind::ThrottledTier
                                : ProductKind::ContentSocial;
    } else {
        p.product = pick < 22   ? ProductKind::DataBundle
                    : pick < 32 ? ProductKind::VoiceAndData
                    : pick < 40 ? ProductKind::TimeBased
                    : pick < 48 ? ProductKind::SharedBundle
                    : pick < 55 ? ProductKind::Roaming
                    : pick < 61 ? ProductKind::ServiceUnits
                    : pick < 70 ? ProductKind::Sms
                    : pick < 75 ? ProductKind::Mms
                    : pick < 82 ? ProductKind::VoiceStep
                    : pick < 88 ? ProductKind::ContentVideo
                    : pick < 92 ? ProductKind::ContentSocial
                    : pick < 96 ? ProductKind::ContentMusic
                                : ProductKind::ThrottledTier;
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
        // ADR-0342: one rating group per charged product. This is how 5G expresses content
        // charging -- a PCC rule binds a service data flow to a rating group -- so distinct
        // content classes are distinct rating groups, not a free-text label.
        case ProductKind::Sms:
            p.rating_group = 5;
            break;
        case ProductKind::Mms:
            p.rating_group = 6;
            break;
        case ProductKind::VoiceStep:
            p.rating_group = 7;
            // A real operator bills an initial block then increments. The generator asks for this
            // much time per Update rather than one lump, which is what makes it STEP charging.
            p.voice_step_seconds = 60;
            break;
        case ProductKind::ContentVideo:
            p.rating_group = 8;
            p.service_id = "video";
            break;
        case ProductKind::ContentSocial:
            p.rating_group = 9;
            p.service_id = "social";
            break;
        case ProductKind::ContentMusic:
            p.rating_group = 10;
            p.service_id = "music";
            break;
        case ProductKind::ThrottledTier:
            p.rating_group = 11;
            p.throttle_threshold_octets = 2000000000ULL;
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

    // ADR-0343: enterprise hierarchy -- enterprise -> cost centre -> line.
    //
    // 50 lines per enterprise, 5 cost centres of 10. Both numbers are chosen to make the structure
    // visible in the data without any one enterprise dominating: at 20k subscribers that is ~80
    // enterprises with ~400 cost centres, which is a realistic shape for a mid-size operator's
    // B2B base rather than one giant account.
    if (p.segment == Segment::Enterprise) {
        const int ent = subscriber_index / 50;
        const int cc = (subscriber_index % 50) / 10;
        p.enterprise_id = "ent-" + std::to_string(ent);
        p.department_id = p.enterprise_id + "-cc-" + std::to_string(cc);
    }

    // Shared bundles pool at a LEVEL, not across a flat list of strangers.
    //
    // Enterprise: the cost centre's pool, so a department's spend is attributable to it -- which
    // is the whole reason a corporate customer wants a hierarchy rather than one company-wide
    // bucket they cannot break down.
    //
    // Consumer: the household, 4 lines. A family plan and a corporate pool are the same mechanism
    // (one TMF654 bucket, several relatedParty members) at different scales, which is why both are
    // expressed here rather than as two features.
    if (p.product == ProductKind::SharedBundle && total_subscribers > 0) {
        p.shared_bucket = true;
        if (p.segment == Segment::Enterprise) {
            p.bucket_key = p.department_id;
        } else {
            const int group = subscriber_index / 4;
            p.bucket_key = "fam-" + std::to_string(group);
            p.bucket_owner_supi = std::to_string((group * 4) % total_subscribers);
        }
    }

    return p;
}

} // namespace cdrgen
