#pragma once

#include <mutex>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "bss_sid/balance.hpp"

// Private to bss/balance-management. Real PostgreSQL persistence (libpqxx) -- see schema.sql's
// own header comment for why this uses PostgreSQL alone (no Redis hot-path layer) for Bucket's
// authoritative remaining_value/reserved_value: a single-statement atomic
// `UPDATE ... WHERE remaining_value >= $amount` already gives the real, provable strong
// consistency CHARGING_PROMPT.md's P4.3 explicitly requires ("prove it under concurrent debit
// tests"), via PostgreSQL's own row-level locking -- no risk of a two-store desync under a crash
// mid-mutation.
//
// One shared `pqxx::connection` serialized behind a mutex -- same "one shared handle, one mutex"
// discipline this project already applied to sbi_core::http2::Client (ADR-0051) and
// bss/product-catalog's own store (ADR-0054), since libpqxx::connection has no built-in pooling
// (unlike sw::redis::Redis, confirmed when nfs/chf's stores were given real Redis persistence).

namespace balance_management {

// Result of a mutation attempt: whether it succeeded (real business outcome -- e.g. insufficient
// balance -- not an error), and the resulting resource record (real ActionStatusType status:
// "completed" or "failed").
template <typename T> struct MutationResult {
    T record;
    bool succeeded;
};

class BalanceStore {
public:
    explicit BalanceStore(std::string resource_base_url, const std::string& conninfo);

    std::optional<bss_sid::Bucket> get_bucket(const std::string& id);
    std::vector<bss_sid::Bucket> list_buckets();

    // ADR-0307 (C1 of ADR-0300): the SHARED bucket a subscriber draws from, if any.
    //
    // No new resource and no schema change were needed -- TMF654's `Bucket` already carries
    // `isShared` and `relatedParty`, which is exactly this concept, and both columns already
    // existed. A family/group/enterprise bucket is one with `is_shared = true` whose
    // `related_party` array names its members.
    //
    // Returns std::nullopt when the subscriber belongs to no shared bucket, which is the ordinary
    // case and is what keeps every existing per-SUPI bucket working untouched.
    std::optional<bss_sid::Bucket> find_shared_bucket_for(const std::string& party_id);

    // Real TMF654 AccumulatedBalance query: aggregates every Bucket belonging to party_account_id
    // into a single totalBalance. Disclosed simplification: sums remainingValue only (not
    // reservedValue), and assumes a single currency/unit across the party's buckets (no real
    // multi-currency conversion exists in this codebase) -- if buckets use different units, the
    // sum is not meaningful; this is flagged in the response's own `description` field rather than
    // silently producing a wrong number, see store.cpp.
    bss_sid::AccumulatedBalance get_accumulated_balance(const std::string& party_account_id);

    // Real TMF654 TopupBalance (credit). If request.bucket->id does not already exist, a new
    // Bucket is created with that id and remainingValue = amount (this project's own disclosed
    // interpretation of "a reference to the bucket impacted by the request -- or the value
    // itself", since the real TMF654 API has no POST /bucket at all -- see
    // bss_sid/balance.hpp's own file header). Always succeeds (a topup/credit has no balance
    // floor to violate).
    MutationResult<bss_sid::TopupBalance> topup(bss_sid::TopupBalance request);
    std::optional<bss_sid::TopupBalance> get_topup(const std::string& id);

    // Real TMF654 AdjustBalance (signed debit/credit). Positive amount credits; negative amount
    // debits and atomically fails (succeeded=false, status="failed") if it would take
    // remainingValue below zero -- the real strong-consistency guarantee.
    MutationResult<bss_sid::AdjustBalance> adjust(bss_sid::AdjustBalance request);
    std::optional<bss_sid::AdjustBalance> get_adjust(const std::string& id);

    // Real TMF654 ReserveBalance. Positive amount reserves (moves remainingValue ->
    // reservedValue, atomically fails if insufficient remainingValue); negative amount unreserves/
    // refunds (moves reservedValue -> remainingValue, atomically fails if insufficient
    // reservedValue). Sign convention is this project's own disclosed interpretation -- the real
    // TMF654 spec text names both "Reserve" and "Unreserve" as real operations on this same
    // resource but does not document the mechanism distinguishing them (see
    // bss_sid/balance.hpp's own file header).
    MutationResult<bss_sid::ReserveBalance> reserve(bss_sid::ReserveBalance request);
    std::optional<bss_sid::ReserveBalance> get_reserve(const std::string& id);

private:
    std::string resource_base_url_;
    std::mutex mutex_;
    pqxx::connection conn_;
};

} // namespace balance_management
