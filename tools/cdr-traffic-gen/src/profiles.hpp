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
    // ADR-0342: the rest of a real operator's catalogue. Each maps to a charging-information
    // block TS 32.291 actually defines, not a label invented here.
    Sms,          // sMSChargingInformation, per-message serviceSpecificUnits
    Mms,          // mMSChargingInformation, sized messages
    VoiceStep,    // mMTelChargingInformation + time granted in STEPS, not one lump
    ContentVideo, // UsedUnitContainer.serviceId -- content / service-class charging
    ContentSocial,
    ContentMusic,
    ThrottledTier, // fair-use: post-threshold traffic rates on a different tier
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

    // ADR-0342: which TS 32.291 charging-information block this product populates, and the
    // service class for content charging. Empty service_id means "not content-charged".
    std::string service_id;
    // Voice step charging: seconds granted per step. 0 = not step-charged. Real operators bill an
    // initial block then increments; one lump grant cannot express that.
    int voice_step_seconds = 0;
    // Fair-use: the volume after which the subscriber moves to the throttled tier.
    std::uint64_t throttle_threshold_octets = 0;

    // ADR-0343: enterprise hierarchy. Real corporate accounts are three levels -- the enterprise,
    // its cost centres, and the lines inside them -- and a shared bundle is pooled at a level, not
    // across a flat list of strangers. `tenantIdentifier` is a real top-level TS 32.291 field that
    // CHF already extracts, so the enterprise travels on every charging request and can scope an
    // offering like any other attribute.
    //
    // Empty for consumers: a consumer has no tenant, and sending one would assert a corporate
    // relationship that does not exist.
    std::string enterprise_id; // tenantIdentifier, e.g. "ent-0042"
    std::string department_id; // cost centre, e.g. "ent-0042-cc-3"
    // The bucket this line draws on. For enterprises that is the DEPARTMENT's pool, so a cost
    // centre's spend is attributable to it; for consumer family plans it is the household owner.
    std::string bucket_key;
};

// Deterministic: the same index always yields the same profile, in any worker and any process.
Profile profile_for(int subscriber_index, int total_subscribers);

const char* to_string(Segment s);
const char* to_string(ProductKind p);

} // namespace cdrgen
