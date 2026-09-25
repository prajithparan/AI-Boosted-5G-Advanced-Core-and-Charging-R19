#pragma once

// ADR-0383: the UDM's subscriber authentication data comes from the UDR over Nudr, as TS 29.505
// designs it -- QueryAuthSubsData (GET .../authentication-data/authentication-subscription) to read
// K/OPc/SQN/AMF/method, and ModifyAuthenticationSubscription (RFC 6902 PATCH, the resource's only
// write operation) to advance the SQN. Replaces the UDM's in-memory store with its two hardcoded
// TS 35.207 subscribers (now seeded in the UDR instead), which is what kept a subscriber onboarded
// through bss/provisioning (ADR-0382) from authenticating.
//
// SQN concurrency: every advance is a compare-and-swap -- one PATCH carrying an RFC 6902 `test` op
// on /sequenceNumber/sqn (the value just read, byte-for-byte) followed by a `replace`. If another
// UDM request (or replica) advanced it first, the UDR rejects the whole patch and this re-reads and
// retries, so two concurrent vectors never share an SQN.
//
// K/OPc are never cached here and never logged; the UDR's GET and PATCH responses both carry them.

#include <aka_crypto/milenage.hpp>
#include <cstdint>
#include <sbi_core/http2_client.hpp>
#include <sbi_core/oauth2_client.hpp>
#include <string>
#include <tl/expected.hpp>

#include "stores.hpp"

namespace udm {

enum class AuthDataError {
    NotFound,    // UDR has no authentication-subscription for this SUPI -> 404
    Unavailable, // UDR unreachable, token failure, or CAS retries exhausted -> 503
    Invalid,     // UDR returned a document the AKA path cannot use -> 500
};

// Advances a 48-bit SQN by n, mod 2^48.
aka_crypto::Sqn sqn_add(const aka_crypto::Sqn& sqn, std::uint64_t n);

class UdrAuthSubscriptionSource {
public:
    UdrAuthSubscriptionSource(sbi_core::http2::Client& client,
                              sbi_core::OAuth2Client& oauth,
                              std::string udr_base_url);

    // Returns the subscriber's data with the SQN to use for the FIRST of `count` vectors, and
    // atomically advances the stored SQN by `count` (vector i uses sqn_add(returned.sqn, i)).
    tl::expected<AuthenticationSubscription, AuthDataError>
    get_and_advance_sqn(const std::string& supi, std::uint64_t count = 1);

    // Verifies AUTS against the subscriber's K/OPc (TS 24.501 9.11.3.1) and, iff genuine, stores
    // SQN_MS + 2^16 (same rule and rationale as ADR-0037). true = resynchronised, false = AUTS
    // failed to verify (stored SQN untouched).
    tl::expected<bool, AuthDataError> resync_sqn(const std::string& supi,
                                                 const aka_crypto::Key128& rand,
                                                 const aka_crypto::Auts& auts);

private:
    struct Fetched {
        AuthenticationSubscription sub;
        std::string sqn_text; // exactly as stored, for the CAS `test` op
    };
    tl::expected<Fetched, AuthDataError> fetch(const std::string& supi);
    // true = swapped, false = lost the race (re-read and retry), error = UDR failure.
    tl::expected<bool, AuthDataError> compare_and_swap_sqn(const std::string& supi,
                                                           const std::string& expected_sqn,
                                                           const aka_crypto::Sqn& new_sqn);
    std::string resource_url(const std::string& supi) const;

    sbi_core::http2::Client& client_;
    sbi_core::OAuth2Client& oauth_;
    std::string udr_base_url_;
};

} // namespace udm
