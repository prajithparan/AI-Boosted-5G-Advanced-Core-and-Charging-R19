#pragma once

// P4.5/ADR-0060 (Stage 3): the single, shared charging code path CHARGING_PROMPT.md's own P4.5
// explicitly demands ("normalising to the SAME internal JSON representation used by the 5G
// path... prove the single-code-path property with a test that charges an identical usage event
// arriving via Gy and via Nchf and asserts an identical rated result"). Every real decision this
// module makes (rating, balance reservation, CDR write, RatingDecision audit) is IDENTICAL
// regardless of whether the caller is `Nchf_ConvergedCharging`'s real HTTP handler (main.cpp) or
// the real Diameter Gy CCR handler (diameter_server.cpp) -- both call these same functions, not
// two parallel implementations that happen to look similar. `MultipleUnitUsage`/
// `MultipleUnitInformation` (TS 32.291's own real types) are themselves already protocol-neutral --
// Gy's own CCR/CCA AVPs are translated into/out of these same types at the Diameter boundary
// (diameter_server.cpp), not given a second, separate internal representation.
//
// Extracted from main.cpp (previously anonymous-namespace-local, HTTP-handler-only) so
// diameter_server.cpp can call the identical functions.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/metrics.hpp"

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

// TS29594_Nchf_SpendingLimitControl's own types now live in TS26510_CommonData_grp.hpp -- see
// stores.hpp's own comment (ADR-0072).
#include "TS26510_CommonData_grp.hpp"
#include "ai_inference.hpp"
#include "bss_sid/balance.hpp"
#include "bss_sid/product.hpp"
#include "cdr.hpp"
#include "rating_decision_store.hpp"
#include "stores.hpp"

namespace chf {

// CHF's own client base URLs/paths for its two real BSS-layer dependencies (ADR-0047/ADR-0056).
// Real, disclosed bug found and fixed while live-verifying the P4.5/ADR-0061 docker-compose.yml
// fix: these were hardcoded to 127.0.0.1, which only works when every NF runs on the same host
// (this project's own original lab convention) -- it can never work once product-catalog/
// balance-management are separate containers, since 127.0.0.1 inside CHF's own container is CHF
// itself, not another container. Same getenv-based-config precedent as every other CHF connection
// string (chf_redis_conninfo/chf_doris_options/chf_rating_postgres_conninfo, main.cpp) --
// CHF_PRODUCT_CATALOG_BASE/CHF_BALANCE_MANAGEMENT_BASE, defaulting to the same real 127.0.0.1 URLs
// for same-host lab runs so nothing existing breaks.
std::string product_catalog_base();
std::string balance_management_base();
constexpr const char* kProductCatalogApiRoot = "/tmf-api/productCatalogManagement/v4";
constexpr const char* kBalanceManagementApiRoot = "/tmf-api/prepayBalanceManagement/v4";

// P4.3 (ADR-0057): a rating decision is now a real (GrantedUnit, cost) pair -- the quantity of
// service granted (e.g. 5GB) AND its real monetary cost (e.g. $20), confirmed as two genuinely
// separate real TMF620 fields on ProductOfferingPrice (`unitOfMeasure` vs `price`), not something
// this project invented a split for. `cost` is nullopt when the matched price has no `price`
// field set (a real, valid TMF620 state -- not every price is monetary), in which case no real
// balance reservation is attempted for this grant (same as before this ADR).
// P4.5/ADR-0060 (E5): tariffId/tariffVersion/offeringName/priceName added so callers can write a
// real RatingDecision audit row (principle 2: "every charge is explainable") without a second
// lookup -- empty when no offering/price was matched (nothing was rated, nothing to audit).
struct RatingResult {
    std::optional<sbi_gen::GrantedUnit> grant;
    std::optional<bss_sid::Money> cost;
    std::optional<std::string> tariffId;
    std::optional<std::string> tariffVersion;
    std::optional<std::string> offeringName;
    std::optional<std::string> priceName;
    // ADR-0072 (gap-closure: real N40 product-configurability). Real TS 32.291
    // MultipleUnitInformation quota-policy fields (§ struct MultipleUnitInformation), populated
    // from the matched ProductOfferingPrice's own real `prodSpecCharValueUse` characteristics (see
    // find_characteristic_value's own comment for why TMF620's generic characteristic mechanism,
    // not a native TMF620 field, is the real, correct place for these 3GPP-charging-specific
    // values to live on a TM Forum resource). std::nullopt when the matched price doesn't
    // configure that particular characteristic -- a real, valid "operator didn't set a policy for
    // this" state, not an error.
    std::optional<std::int64_t> validityTimeSec;
    std::optional<std::int64_t> quotaHoldingTimeSec;
    std::optional<std::int64_t> volumeQuotaThreshold;
    std::optional<std::int64_t> timeQuotaThreshold;
    std::optional<std::int64_t> unitQuotaThreshold;
    // P4.8 (ADR-0074): present only when AiQuotaSizer actually adjusted this grant -- model
    // id/version, input feature vector, predicted usage, and the deterministic clamp that was
    // applied. std::nullopt in every other case (AI disabled, cold start, non-volume grant,
    // latency budget exceeded) -- a real, valid "no advisory" state, not an error. Flows straight
    // into RatingDecisionRecord::aiAdvisory (write_rating_decision below) for governance logging.
    std::optional<nlohmann::json> aiAdvisory;
};

// UPDATE (ADR-0072, gap-closure: real N40 product-configurability). Real, fixed correctness gap:
// this used to pick the first Active/isSellable ProductOffering with a price REGARDLESS of the
// request's own real `ratingGroup` -- meaning every rating group billed identically, real
// per-service/per-product differentiation never actually happened despite CHF->product-catalog
// wiring existing since ADR-0048. Now real-matches on a `ratingGroup` characteristic
// (`prodSpecCharValueUse`, see find_characteristic_value's own comment) -- the FIRST
// Active/isSellable offering whose price's own `ratingGroup` characteristic value equals
// `rating_group` wins; a price with no `ratingGroup` characteristic configured is never matched
// (real, disclosed: an unconfigured price grants nothing for any rating group, rather than
// ambiguously matching everything). Falls back to granting nothing (schema-valid empty grant,
// same as always) if no price configures this rating group.
//
// Also populates RatingResult's own real quota-policy fields (validityTimeSec/
// quotaHoldingTimeSec/volumeQuotaThreshold/timeQuotaThreshold/unitQuotaThreshold) from the SAME
// matched price's own `prodSpecCharValueUse` entries, when configured.
//
// No OAuth2 token needed: product-catalog is mTLS-only (ADR-0047), same trust boundary the client
// cert already provides.
//
// Unit conversion is real but deliberately narrow: TS 32.291's GrantedUnit has no generic "amount
// + unit string" field the way TMF620's Quantity does -- totalVolume/uplinkVolume/downlinkVolume
// are raw octet counts (TS 32.298's own CDR volume fields are octet-counted, confirmed by their
// Uint64 typing with no separate unit field), time is raw seconds. Only "GB"/"MB" (decimal,
// matching 3GPP's own octet-counting convention, not binary GiB/MiB) convert to totalVolume; any
// other unit string falls back to serviceSpecificUnits carrying the raw amount unconverted --
// disclosed as a real but narrow conversion, not a general unit-aware rating engine.
//
// P4.8 (ADR-0074): supi/ai_quota_sizer/quota_feature_store are all optional (default empty/
// nullptr -- every existing caller before this ADR keeps compiling and behaving identically).
// When all three are real (non-empty supi, an enabled AiQuotaSizer, a QuotaFeatureStore with
// prior history for this SUPI+ratingGroup) AND the matched price grants totalVolume (the
// "narrow conversion" case above -- serviceSpecificUnits grants are not AI-adjusted, a real,
// disclosed scope choice, not an oversight), the AI-predicted usage is turned into a
// DETERMINISTIC multiplier clamped to [0.5x, 2.0x] of the price-configured grant and applied to
// it -- see charging_engine.cpp's own implementation comment for the exact deterministic rule.
// "This model informs the decision. The deterministic rating engine makes it."
RatingResult build_rating_grant(sbi_core::http2::Client& catalog_client,
                                std::int64_t rating_group,
                                const std::string& supi = "",
                                AiQuotaSizer* ai_quota_sizer = nullptr,
                                QuotaFeatureStore* quota_feature_store = nullptr);

// P4.3 (ADR-0057): CHF as a real HTTP client of bss/balance-management (ADR-0056) -- reserves
// `cost` against the real per-subscriber Bucket keyed by SUPI (this project's own disclosed
// convention: no real customer-to-bucket provisioning system exists, see
// bss/balance-management's own file header). Returns false if the reservation was rejected
// (insufficient balance, or no bucket exists yet for this SUPI -- same real business outcome
// either way) -- callers must not grant units when this returns false, the real prepaid-
// enforcement point this ADR closes (ADR-0048/0050's own disclosed "no balance/wallet deduction"
// gap).
bool reserve_subscriber_balance(sbi_core::http2::Client& balance_client,
                                const std::string& supi,
                                const bss_sid::Money& cost,
                                const std::string& description);

// P4.3 (ADR-0057): Release-time finalization -- unreserves everything this session held and turns
// `amount_to_debit` of it into a real, permanent debit. See charging_engine.cpp's own comment for
// the real order-dependent unreserve-then-debit bug this function's implementation was fixed for.
//
// ADR-0297: `amount_to_debit` is a SEPARATE parameter from `total_reserved` -- that separation is
// the whole fix. Before it, the two were necessarily equal and a subscriber who used 1 GB of a
// 5 GB reservation was charged for 5 GB. The reservation must still be released in full (it is
// what the subscriber's balance is actually holding), but only what was consumed may be debited.
// A caller that genuinely cannot measure consumption passes the full reserved total and says why.
void finalize_subscriber_balance(sbi_core::http2::Client& balance_client,
                                 const std::string& supi,
                                 double total_reserved,
                                 double amount_to_debit,
                                 const std::string& description);

// ADR-0297: what fraction of a session's grant was actually consumed, and therefore what part of
// its reservation may be debited.
//
// Volume and service-specific units are proportioned INDEPENDENTLY against their own grants and
// the results combined by weighting each dimension by nothing more than its own share of the
// grant -- because this project's rating engine grants exactly one dimension per rating group, a
// real session normally exercises exactly one branch here, and the mixed case is handled rather
// than assumed away.
//
// Clamped to [0, total_reserved]: usage exceeding the grant cannot debit more than was reserved
// (the subscriber's balance never held more than that), and a missing or zero grant means no
// proportion can be computed, in which case the FULL reserved amount is returned rather than zero
// -- failing toward charging what was reserved, not toward giving usage away.
double proportional_debit(double total_reserved,
                          double granted_volume,
                          double used_volume,
                          double granted_service_units,
                          double used_service_units);

// P4.4/ADR-0058: writes one real CDR row (chf::CdrRecord, see cdr.hpp) per MultipleUnitUsage
// entry -- every field here is either a real TS 32.291 value already flowing through the caller,
// or a real ADR-0057 rating-engine output (grant/cost), not fabricated. Shared between every
// caller of charge_one_usage below.
//
// `invocation_time_stamp` is the real TS 32.291 `invocationTimeStamp` the consumer actually sent,
// already parsed to a time_t (sbi_core::parse_rfc3339_to_time_t). Pass std::nullopt when the
// caller genuinely has no consumer-supplied event time -- the Diameter Gy and CAP paths do, since
// neither RFC 4006 CCR handling nor cap_server.cpp decodes an event timestamp in this build --
// and the CDR falls back to write time, which is what every caller did unconditionally before.
// That fallback is a real, disclosed approximation, not a claim the consumer sent this value.
void write_converged_charging_cdr(chf::CdrWriter& cdr_writer,
                                  const std::string& ref,
                                  const std::string& operation,
                                  const std::string& supi,
                                  const std::string& node_functionality,
                                  const std::string& recording_network_function_id,
                                  std::int64_t invocation_sequence_number,
                                  const sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging& usage,
                                  const RatingResult& rating,
                                  bool reserved,
                                  std::optional<std::time_t> invocation_time_stamp = std::nullopt);

// P4.5/ADR-0060 (E5): writes one real RatingDecision audit row per MultipleUnitUsage entry that
// was actually rated (rating.tariffId has a value) -- an unmatched offering/price is not a
// "decision" worth auditing (nothing was explained because nothing was rated). Shared between
// every caller of charge_one_usage below.
void write_rating_decision(chf::RatingDecisionStore& rating_decision_store,
                           const std::string& ref,
                           const sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging& usage,
                           const RatingResult& rating,
                           bool reserved);

struct ChargeUsageResult {
    RatingResult rating;
    bool reserved = true;
};

// The single, shared code path: rating, balance reservation, CDR write, RatingDecision audit --
// called identically by Nchf_ConvergedCharging's real HTTP Create/Update handlers (main.cpp) and
// the real Diameter Gy CCR handler (diameter_server.cpp).
//
// P4.8 (ADR-0074): ai_quota_sizer/quota_feature_store default to nullptr -- the Diameter Gy
// (diameter_server.cpp) and CAP gsmSCF (cap_server.cpp) call sites are left unchanged and stay
// deterministic-only, a real, disclosed scope choice: P4.8's own success metric (reduction in
// Nchf round-trips) is specifically about the HTTP Nchf_ConvergedCharging path, so only main.cpp's
// real Create/Update handlers pass the real pointers.
ChargeUsageResult
charge_one_usage(sbi_core::http2::Client& catalog_client,
                 sbi_core::http2::Client& balance_client,
                 chf::CdrWriter& cdr_writer,
                 chf::RatingDecisionStore& rating_decision_store,
                 chf::ChargingDataStore& charging_data_store,
                 opentelemetry::metrics::Counter<std::uint64_t>* grant_counter,
                 opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter,
                 const std::string& ref,
                 const std::string& operation,
                 const std::string& supi,
                 const std::string& node_functionality,
                 const std::string& recording_network_function_id,
                 std::int64_t invocation_sequence_number,
                 const sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging& usage,
                 chf::AiQuotaSizer* ai_quota_sizer = nullptr,
                 chf::QuotaFeatureStore* quota_feature_store = nullptr,
                 std::optional<std::time_t> invocation_time_stamp = std::nullopt);

// P4.2/ADR-0055, TS 29.594 (Nchf_SpendingLimitControl): builds the real SpendingLimitStatus both
// Subscribe/Update return, per the real confirmed schema. Extracted from main.cpp (was anonymous-
// namespace-local) alongside the P4.5/ADR-0059 Stage 4 (Sy half) work, so diameter_server.cpp's
// real SLR/SLA handler can call the exact same function the HTTP Subscribe/Update handlers use --
// the same single-code-path property Stage 3/4's Gy/Rf work already established, applied to Sy.
//
// UPDATE (ADR-0072, gap-closure: real N28 end-to-end): `currentStatus` is no longer a hardcoded
// placeholder -- it's a real, per-policyCounterId lookup against `config_store` (this project's
// own real, disclosed operator/GUI-facing config surface, since TS29594's own spec text leaves the
// actual status values "not specified... out of scope of 3GPP", so this project must define its
// own real source for them rather than inventing per-call values). A policyCounterId with no
// configured status falls back to "unknown" -- the same least-invented placeholder as before, now
// only for the genuinely-unconfigured case rather than always.
sbi_gen::SpendingLimitStatus
build_spending_limit_status(const sbi_gen::SpendingLimitContext& context,
                            PolicyCounterConfigStore& config_store);

} // namespace chf
