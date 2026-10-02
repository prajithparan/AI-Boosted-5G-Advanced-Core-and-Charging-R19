#pragma once

#include "sbi_core/http2_client.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "bss_sid/product.hpp"

// ADR-0445/ADR-0446 increment 1, step 3: the in-memory catalog/policy snapshot that removes
// `build_rating_grant`'s N+1 `GET /productOfferingPrice/{id}` loop (ADR-0445 finding #1).
//
// Deliberately a DUMB cache: this class knows nothing about isSellable/Active/ratingGroup/
// chargingScope/unitOfMeasure -- it just mirrors the real `GET /productOffering` and
// `GET /productOfferingPrice` collections (the bulk routes ADR-0446 confirmed already exist) into
// memory, preserving the offerings' original order (the real "first match wins" semantics
// `build_rating_grant` depends on, see `charging_engine.cpp`'s own header comment). Every business
// rule stays in `charging_engine.cpp`'s `try_rate_against`, applied identically whether its
// (offering, price) pair came from this snapshot or from a live fetch -- so this class cannot
// silently change what gets rated, only how the data for rating was obtained.
namespace chf {

struct CatalogCandidate {
    bss_sid::ProductOffering offering;
    bss_sid::ProductOfferingPrice price;
};

class CatalogSnapshot {
public:
    // Fetches both collections once and atomically replaces the cached candidate list on success.
    // Returns false and leaves the previous (possibly empty, possibly stale) snapshot in place on
    // any failure -- a transient catalog outage must not blank out an otherwise-working cache, the
    // same graceful-degradation principle this file's sibling stores already follow. Safe to call
    // from any thread; `candidates()` always sees either the old or the new list, never a partial
    // one.
    bool refresh(sbi_core::http2::Client& catalog_client);

    // True once at least one refresh has succeeded. Callers that want a hard "not ready yet" gate
    // can check this; `build_rating_grant` itself does not require it (see its own comment) --
    // an unready/never-refreshed snapshot behaves like an empty catalog, which is byte-identical to
    // "no offerings configured" that this code already handles (grants nothing, not an error).
    bool ready() const { return ready_.load(std::memory_order_acquire); }

    // Monotonically increasing, bumped on every successful refresh -- exposed as a metric so a
    // reader can see the cache is actually turning over, not silently frozen.
    std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

    // Seconds since the last successful refresh, or -1 if none has ever succeeded.
    double age_seconds() const;

    // A point-in-time copy of the current candidate list, in the real collection's own order.
    // Copying rather than returning a reference under lock: the list is a real operator's catalog
    // (tens to low thousands of entries), not a reason to hold a mutex across a caller's whole
    // matching scan, and a charging request must never block behind a concurrent refresh.
    std::vector<CatalogCandidate> candidates() const;

private:
    mutable std::mutex mutex_;
    std::vector<CatalogCandidate> candidates_;
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<std::int64_t> last_refresh_unix_ms_{-1};
};

} // namespace chf
