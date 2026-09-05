#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <utility>
#include <vector>

// TS29594_Nchf_SpendingLimitControl's own real types (SpendingLimitContext/Status/
// PolicyCounterInfo/etc.) now live in TS26510_CommonData_grp.hpp -- adding
// TS29519_Policy_Data.yaml as a codegen pilot file (ADR-0072) created a new file-level cross-
// reference cycle that pulled TS29594's own schemas into the shared SCC group; see
// libs/sbi-generated/CMakeLists.txt's own comment on why a stale include here would otherwise
// silently keep compiling against last build's now-wrong header name.
#include "TS26510_CommonData_grp.hpp"

// Private to nfs/chf -- not shared with any other NF, per CLAUDE.md's "no NF includes another
// NF's private headers" rule.
//
// Real Redis/Valkey persistence (redis-plus-plus), replacing this file's earlier in-memory-only
// stores -- CHARGING_PROMPT.md's entity E3 (Session Establishment) explicitly requires charging
// sessions to be "idempotent and recoverable across restarts and network partitions", and
// docs/DATA_MODEL.md's own E3 persistence assignment is Redis/Valkey for exactly this reason.
// `sw::redis::Redis` manages its own internal connection pool and IS genuinely thread-safe for
// concurrent use (confirmed by reading its own header, not assumed) -- unlike
// bss/product-catalog's single-pqxx-connection-behind-a-mutex pattern (ADR-0054), no mutex is
// needed here.
//
// Real, disclosed limitation: ID generation uses Redis INCR (atomic, survives restart, safe
// across multiple CHF instances sharing the same Redis) instead of the earlier process-local
// counter -- a genuine improvement, not just a persistence bolt-on, since the old counter would
// have collided across CHF replicas or reset to 1 on every restart.

namespace chf {

class ChargingDataStore {
public:
    explicit ChargingDataStore(std::shared_ptr<sw::redis::Redis> redis)
        : redis_(std::move(redis)) {}

    // Allocates a new ChargingDataRef, marks it active, and records the real per-session content
    // P4.3's real ABMF integration needs (nfs/chf/src/main.cpp's own header comment): which
    // subscriber this session belongs to (a real per-SUPI Bucket in bss/balance-management, see
    // ADR-0056/0057), and the running total already reserved against that bucket for this
    // session. Extended from the earlier active-ref-only shape (ADR-0055) since Update/Release now
    // need this real content, not just whether the ref exists.
    std::string create(const std::string& supi);

    // Returns false (and leaves state unchanged) if ref isn't currently active -- an unknown or
    // already-released ChargingDataRef, per TS 32.291's real 404 case for Update/Release.
    bool release(const std::string& ref);

    // ADR-0050 Stage 4: Update's real 404 case (TS 32.291: an unknown/already-released
    // ChargingDataRef) needs a non-destructive check -- unlike release(), Update must NOT remove
    // the ref just for asking whether it's still active.
    bool is_active(const std::string& ref);

    std::optional<std::string> get_supi(const std::string& ref);

    // Real atomic accumulator (Redis HINCRBYFLOAT -- no read-then-write race between concurrent
    // Update calls on the same ref) tracking how much has been reserved (bss_sid ReserveBalance,
    // ADR-0056) against this session's bucket so far. Release finalizes exactly this total as a
    // real permanent debit (ADR-0057).
    double add_reserved(const std::string& ref, double amount);
    double get_reserved_total(const std::string& ref);

    // ADR-0297: the units GRANTED alongside the money reserved for them, accumulated by the same
    // real atomic HINCRBYFLOAT. Release needs both to charge proportionally: money alone cannot
    // say what fraction of a grant was consumed.
    //
    // Two dimensions, tracked separately and never summed, because they are not commensurable --
    // this project's rating engine grants EITHER `totalVolume` (octets, from a GB/MB price) OR
    // `serviceSpecificUnits`, never both for one rating group, and adding octets to service units
    // would produce a ratio that means nothing. A session that somehow mixed them proportions each
    // dimension against its own grant.
    // ADR-0304: the third dimension. Duration grants (GrantedUnit.time) are what make CAP's
    // ApplyChargingReport -- which reports elapsed TIME -- proportionable at all.
    void add_granted_time(const std::string& ref, double seconds);
    double get_granted_time(const std::string& ref);
    void add_granted_volume(const std::string& ref, double octets);
    void add_granted_service_units(const std::string& ref, double units);
    double get_granted_volume(const std::string& ref);
    double get_granted_service_units(const std::string& ref);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

// P4.2 (ADR-0055): Nchf_OfflineOnlyCharging's own OfflineChargingDataRef resource collection --
// a genuinely separate 3GPP resource from ChargingDataRef above (different service,
// /offlinechargingdata not /chargingdata), not reused, even though the tracking shape is
// identical -- no rating engine involved here, per TS32291_Nchf_OfflineOnlyCharging.yaml's own
// ChargingDataResponse schema carrying no multipleUnitInformation/grantedUnit field at all.
class OfflineChargingDataStore {
public:
    explicit OfflineChargingDataStore(std::shared_ptr<sw::redis::Redis> redis)
        : redis_(std::move(redis)) {}

    std::string create();
    bool release(const std::string& ref);
    bool is_active(const std::string& ref);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

// P4.2 (ADR-0055): Nchf_SpendingLimitControl's subscriptionId resource collection (TS 29.594).
// Unlike ChargingDataStore/OfflineChargingDataStore above, this is a real resource store (holds
// the actual SpendingLimitContext as a JSON string value, not just an active-ref marker) -- PUT
// /subscriptions/{id} is a real update-in-place that needs the previous context, and building
// each SpendingLimitStatus response (Subscribe/Update) needs the subscription's own
// policyCounterIds to enumerate.
class SpendingLimitSubscriptionStore {
public:
    explicit SpendingLimitSubscriptionStore(std::shared_ptr<sw::redis::Redis> redis)
        : redis_(std::move(redis)) {}

    // Server-assigned subscriptionId, matching every other resource-creation convention in this
    // codebase (see e.g. bss/product-catalog/src/store.hpp).
    std::string create(sbi_gen::SpendingLimitContext context);

    // Real update-in-place. Returns false (leaves state unchanged) if id isn't a currently active
    // subscription -- TS 29.594's real 404 case.
    bool update(const std::string& id, sbi_gen::SpendingLimitContext context);

    bool remove(const std::string& id);

    std::optional<sbi_gen::SpendingLimitContext> get(const std::string& id);

    // ADR-0072 (gap-closure: real N28 end-to-end) -- real enumeration of every currently active
    // subscription, needed so a real policy-counter status change (see PolicyCounterConfigStore
    // below) can find and notify every subscriber that named that counter. Real, disclosed cost:
    // O(active subscriptions) full GET per call -- fine at this project's real lab scale, not
    // claimed to be a production-scale design.
    std::vector<std::pair<std::string, sbi_gen::SpendingLimitContext>> list_all();

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

// ADR-0072 (gap-closure: real N28 end-to-end). Real, THIS-PROJECT-OWNED configuration surface for
// policyCounterId -> currentStatus -- NOT a 3GPP-defined resource. TS29594's own spec text is
// explicit that PolicyCounterInfo.currentStatus values "are not specified... out of scope of
// 3GPP" (real, cited, not assumed), so a real operator-facing config surface for it is this
// project's own necessary addition, not a spec deviation -- matching the "configuration
// parameters... to create from GUI later" requirement. Backs both `build_spending_limit_status`'s
// real (no-longer-hardcoded) status lookup and the real statusNotification-push trigger wired into
// main.cpp's own admin/config route.
class PolicyCounterConfigStore {
public:
    explicit PolicyCounterConfigStore(std::shared_ptr<sw::redis::Redis> redis);

    void set_status(const std::string& policy_counter_id, const std::string& status);
    // std::nullopt if never configured -- caller falls back to a real, disclosed default.
    std::optional<std::string> get_status(const std::string& policy_counter_id);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

// P4.8 (CHARGING_PROMPT.md Angle 1a, ADR-0074): rolling per-SUPI/per-ratingGroup consumption
// history -- the real feature source for AiQuotaSizer::predict (nfs/chf/src/ai_inference.hpp).
// Deliberately Redis-backed, not a live Doris query per charging request: querying Doris
// synchronously inside the real-time Nchf_ConvergedCharging path would add
// unpredictable latency to the exact path P4.8's own hard-latency-budget requirement protects --
// this rolling window is the same cheap, already-established hot-path-state pattern every other
// store in this file uses (ChargingDataStore, SpendingLimitSubscriptionStore).
struct QuotaHistorySnapshot {
    std::vector<double> recentUsedVolumes; // most-recent-first, up to 3 entries (octets)
    std::optional<std::int64_t> lastInvocationUnixSec;
    std::optional<double> lastGrantedTotalVolume; // octets
};

class QuotaFeatureStore {
public:
    explicit QuotaFeatureStore(std::shared_ptr<sw::redis::Redis> redis)
        : redis_(std::move(redis)) {}

    // std::nullopt only when there is truly no prior history for this SUPI+ratingGroup (the
    // real, disclosed cold-start case: charging_engine.cpp skips AI-adjusted sizing entirely and
    // falls back to the plain deterministic grant, same as AiQuotaSizer being disabled).
    std::optional<QuotaHistorySnapshot> get(const std::string& supi, std::int64_t rating_group);

    // Call once real usage (usedUnitContainer) has actually been reported for this request --
    // shifts the rolling window and records this request's own granted volume as the baseline
    // the NEXT prediction's multiplier will be measured against.
    void record_usage(const std::string& supi,
                      std::int64_t rating_group,
                      double used_total_volume,
                      std::optional<double> granted_total_volume,
                      std::int64_t invocation_unix_sec);

private:
    std::shared_ptr<sw::redis::Redis> redis_;
};

} // namespace chf
