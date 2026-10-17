#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

// OAuth2 access-token issuance/verification for the Nnrf_AccessToken service
// (specs/5G_APIs-REL-19/TS29510_Nnrf_AccessToken.yaml). Claim names (iss, sub, aud, scope, exp)
// are transcribed verbatim from that file's AccessTokenClaims schema (lines 361-406), not invented.
// ES256 (ECDSA P-256) chosen over RS256: the lab PKI (scripts/gen-lab-pki.sh) already generates
// P-256 keys for mTLS, so ES256 keeps the codebase to one curve/algorithm family instead of two;
// EC keys are also smaller and faster to verify than RSA at an equivalent security level. See
// docs/DECISIONS.md ADR-0012.
//
// Issuer lives in NRF (the only NF that issues tokens). Verifier is reusable by every NF that
// needs to check an incoming Authorization: Bearer token, which is every NF once Phase 2 has more
// than one -- kept in sbi-core rather than nfs/nrf for that reason.

namespace sbi_core::jwt {

struct IssueResult {
    std::string token;
    std::string token_type = "Bearer";
    std::chrono::seconds expires_in;
};

class Issuer {
public:
    // private_key_pem_path: EC P-256 private key (SEC1 or PKCS8 PEM, either works with OpenSSL).
    // issuer_nf_instance_id: NRF's own NfInstanceId (UUID), used as the `iss` claim per spec --
    // AccessTokenClaims.iss is typed as NfInstanceId, not an arbitrary string.
    Issuer(std::string private_key_pem_path, std::string issuer_nf_instance_id);

    // subject_nf_instance_id: the requesting NF's NfInstanceId (`sub`).
    // audience_nf_type: target NFType (`aud`) -- see AccessTokenClaims.aud; this implementation
    // only supports the single-NFType form of `aud`, not the NfInstanceId-list alternative (no
    // caller needs targeted-instance tokens yet; extend when one does, per CLAUDE.md's
    // no-speculative-abstraction rule).
    IssueResult issue(const std::string& subject_nf_instance_id,
                      const std::string& audience_nf_type,
                      const std::string& scope,
                      std::chrono::seconds ttl);

private:
    std::string private_key_pem_;
    std::string issuer_nf_instance_id_;
};

struct VerifyResult {
    bool valid = false;
    std::string subject;  // sub, only meaningful if valid
    std::string audience; // aud, only meaningful if valid (first entry of an array aud)
    // Every aud entry: one NFType, or the NfInstanceId array form (AccessTokenClaims.aud anyOf).
    std::vector<std::string> audiences;
    // AccessTokenClaims' required claims (sub, aud, scope, exp) the token lacks; iss is enforced by
    // the signature check itself. The token is still `valid` (signature and expiry hold) -- the
    // caller decides, e.g. ACCESS_TOKEN_CLAIM_MISSING (TS 29.500 Table 5.2.7.2-1).
    std::vector<std::string> missing_claims;
    std::string scope; // scope, only meaningful if valid
    std::string error; // only meaningful if !valid
};

class Verifier {
public:
    // public_key_pem_path: the EC P-256 public key matching the Issuer's private key.
    Verifier(std::string public_key_pem_path, std::string expected_issuer_nf_instance_id);

    VerifyResult verify(const std::string& token);

private:
    std::string public_key_pem_;
    std::string expected_issuer_;
};

} // namespace sbi_core::jwt
