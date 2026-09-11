#pragma once

// ADR-0341: customer profiles -- consumer and enterprise segments, each exercising a real CHF
// commercial product rather than a single generic data session.
//
// ADR-0340 established the lesson this file exists to apply: a generator that draws every session
// from one distribution produces data with nothing to learn. Structure is what makes a dataset
// worth having, and a real subscriber base has two kinds of it:
//
//   1. PER-SUBSCRIBER PERSISTENCE -- heavy users stay heavy (ADR-0340 fixed this).
//   2. SEGMENT AND PRODUCT STRUCTURE -- an enterprise slice-scoped line and a consumer prepaid
//      bundle differ in volume, in which attributes their sessions carry, and in which offering
//      rates them. That is what this adds.
//
// Every profile is DERIVED from the subscriber index, never stored: any worker computes the same
// profile for the same SUPI with no shared state and no coordination.

#include <cstdint>
#include <string>

namespace cdrgen {

enum class Segment { Consumer, Enterprise };

// The CHF products README.md documents. Each maps to a real rating path: a unit of measure, a
// charging scope, or a rating-group set -- not a label.
enum class ProductKind {
    DataBundle,   // GB/MB -> GrantedUnit.totalVolume
    ServiceUnits, // events/messages -> serviceSpecificUnits
    TimeBased,    // SEC/MIN -> GrantedUnit.time
    VoiceAndData, // one price covering several rating groups (monetary pooling)
    SliceScoped,  // chargingScope on sNSSAI/UPF/DNN -- the enterprise case
    Roaming,      // servingCNPlmnId != hPlmnId
    SharedBundle, // family/group: several members drawing on one bucket
};

struct Profile {
    Segment segment = Segment::Consumer;
    ProductKind product = ProductKind::DataBundle;
    // Natural log of the subscriber's mean session volume. Enterprise lines sit materially higher,
    // which is the single most important structural difference for a usage model.
    double log_mean_volume = 14.0;
    std::int64_t rating_group = 1;
    int slice_sst = 1;
    std::string slice_sd;
    std::string dnn;
    std::string rat_type;
    // Home PLMN, and the serving PLMN for the session. Equal unless the profile roams.
    std::string home_mcc = "999";
    std::string home_mnc = "70";
    std::string serving_mcc = "999";
    std::string serving_mnc = "70";
    std::string charging_characteristics;
    std::uint32_t upf_id = 1;
    // Shared-bundle members resolve to one bucket, so several SUPIs draw on the same balance.
    bool shared_bucket = false;
    std::string bucket_owner_supi;
};

// Deterministic: the same index always yields the same profile, in any worker and any process.
Profile profile_for(int subscriber_index, int total_subscribers);

const char* to_string(Segment s);
const char* to_string(ProductKind p);

} // namespace cdrgen
