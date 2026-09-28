#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>

#include "aka_crypto/kdf.hpp"
#include "nf_config/redis_router.hpp"

// Private to nfs/amf -- not shared with any other NF, per CLAUDE.md's "no NF includes another
// NF's private headers" rule.
//
// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md, task #100/ADR-0075): real, persistent NAS
// security context, keyed by 5G-TMSI -- the foundational piece ServiceRequest (TS 24.501 §5.6.1)
// needs and this project never had. Every NAS security context this project has built so far
// (`ngap_task.cpp`'s own `UeAuthState`) lives ONLY in per-NG-association memory, destroyed the
// moment the SCTP association tears down -- correct for the single-registration-per-association
// scope every prior NGAP/NAS stage disclosed, but it means a UE reconnecting on a FRESH
// association (exactly what ServiceRequest is for) has nothing to reconnect to. Real, disclosed
// architectural gap found via this project's own free5GC/open5GS capability sweep, not assumed.
//
// Redis-backed, same "hot-path state, not a system-of-record" pattern every other NF's own
// Redis-backed store already uses (CHF's ChargingDataStore, PCF's SpendingLimitTrackingStore) --
// this is genuinely hot-path (read+write on every secured NAS message, both the original
// registration flow and any later ServiceRequest), not a place for a slower RDBMS round-trip.

namespace amf {

// TS 33.501's real per-UE NAS security context: KAMF is the root key this project already
// derives (aka_crypto::derive_kamf, ngap_task.cpp's own Stage 3) -- KNASint/KNASenc are
// re-derived from it on load (aka_crypto::derive_knas_int/derive_knas_enc, same functions
// Stage 4 already calls), not separately persisted, so there is exactly one real root-of-trust
// value stored per UE, matching TS 33.501's own key hierarchy rather than inventing a second,
// parallel one.
struct UeSecurityContext {
    std::string supi;
    aka_crypto::Kamf kamf{};
    // TS 24.501 §9.11.3.32 NAS key set identifier -- 0-6, the ngKSI this context was
    // authenticated under. ServiceRequest's own ngKSI (TS 24.501 §9.11.3.32) is checked against
    // this on lookup -- a mismatch means the UE and AMF have desynchronized security contexts
    // (TS 24.501 §5.6.1's own real reject case), not silently accepted.
    std::uint8_t ngksi = 0;
    // Real, persistent, MONOTONICALLY INCREMENTING NAS COUNT (TS 24.501 §4.4.3.1) -- replaces
    // every prior NGAP/NAS stage's own hardcoded per-association literal (downlink_count=0/1/2,
    // uplink_count=0/1) now that a UE's security context genuinely survives across multiple NG
    // associations (registration, then any number of later ServiceRequest-triggered
    // reconnections). Real, disclosed scope: this project's single-registration-per-UE lab scope
    // means COUNT overflow (TS 24.501's own real re-authentication trigger) is not implemented --
    // the same class of disclosed simplification every prior stage already carries.
    std::uint32_t uplink_count = 0;
    std::uint32_t downlink_count = 0;
    // TS 24.501 §9.11.3.54 UE Security Capability, replayed verbatim into any future
    // SecurityModeCommand this context needs (e.g. a real re-authentication flow) -- same real
    // anti-bidding-down-attack requirement RegistrationRequestInfo::ue_security_capability's own
    // comment already documents, now persisted alongside the rest of the context it belongs
    // with instead of living only in per-association memory.
    std::vector<std::uint8_t> ue_security_capability;
};

// docs/DECISIONS.md ADR-0444: Redis-cluster-capable, same real routing UDSF's own store already
// uses (ADR-0443) -- every operation below is single-key (put/get/next_uplink_count/
// next_downlink_count/remove all key off one tmsi's own hash; allocate_tmsi is a single global
// counter), so this store uses RedisRouter::with(f) (no hash tag, no pinned connection), never
// with(hash_tag, f)/transaction(...).
class UeSecurityContextStore {
public:
    explicit UeSecurityContextStore(nf_config::RedisRouter redis) : redis_(std::move(redis)) {}

    // tmsi: the real 5G-TMSI (4 octets, TS 23.003 §2.10) this context is keyed by -- assigned by
    // put_with_fresh_tmsi below at registration time, looked up by decode_service_request's own
    // extracted value on every later ServiceRequest.
    void put(std::uint32_t tmsi, const UeSecurityContext& context);
    std::optional<UeSecurityContext> get(std::uint32_t tmsi);

    // Real atomic increment (Redis HINCRBY -- no read-then-write race between concurrent secured
    // messages for the same UE), returning the PRE-increment value to use for the message just
    // about to be sent/verified -- same real "allocate then use" convention as
    // ChargingDataStore::add_reserved's own atomic counter.
    std::uint32_t next_uplink_count(std::uint32_t tmsi);
    std::uint32_t next_downlink_count(std::uint32_t tmsi);

    void remove(std::uint32_t tmsi);

    // Real, atomic (Redis INCR) fresh-5G-TMSI allocation -- unique across this AMF instance's own
    // lifetime, same real ID-allocation precedent as ChargingDataStore::create's own INCR-based
    // ref allocation. TS 24.501 places no real uniqueness requirement beyond "unique within this
    // AMF" (a UE only ever presents a TMSI back to the AMF that issued it), so this project's
    // own single-AMF-instance lab scope makes a simple incrementing counter correct, not a
    // simplification of a real multi-AMF-instance requirement.
    std::uint32_t allocate_tmsi();

private:
    nf_config::RedisRouter redis_;
};

} // namespace amf
