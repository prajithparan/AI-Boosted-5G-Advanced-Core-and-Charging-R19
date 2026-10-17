#include "sbi_core/jwt.hpp"

#include <fstream>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <sstream>
#include <stdexcept>

namespace sbi_core::jwt {

namespace {

using Traits = ::jwt::traits::nlohmann_json;

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("sbi_core::jwt: cannot open key file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

Issuer::Issuer(std::string private_key_pem_path, std::string issuer_nf_instance_id)
    : private_key_pem_(read_file(private_key_pem_path)),
      issuer_nf_instance_id_(std::move(issuer_nf_instance_id)) {}

IssueResult Issuer::issue(const std::string& subject_nf_instance_id,
                          const std::string& audience_nf_type,
                          const std::string& scope,
                          std::chrono::seconds ttl) {
    const auto now = std::chrono::system_clock::now();

    auto token = ::jwt::create<Traits>()
                     .set_issuer(issuer_nf_instance_id_)
                     .set_subject(subject_nf_instance_id)
                     .set_audience(audience_nf_type)
                     .set_issued_at(now)
                     .set_expires_at(now + ttl)
                     .set_payload_claim("scope", ::jwt::basic_claim<Traits>(scope))
                     .sign(::jwt::algorithm::es256("", private_key_pem_, "", ""));

    return IssueResult{.token = std::move(token), .token_type = "Bearer", .expires_in = ttl};
}

Verifier::Verifier(std::string public_key_pem_path, std::string expected_issuer_nf_instance_id)
    : public_key_pem_(read_file(public_key_pem_path)),
      expected_issuer_(std::move(expected_issuer_nf_instance_id)) {}

VerifyResult Verifier::verify(const std::string& token) {
    VerifyResult result;
    try {
        const auto decoded = ::jwt::decode<Traits>(token);

        const auto verifier =
            ::jwt::verify<Traits>()
                .allow_algorithm(::jwt::algorithm::es256(public_key_pem_, "", "", ""))
                .with_issuer(expected_issuer_);
        verifier.verify(decoded);

        result.valid = true;
        if (decoded.has_subject()) {
            result.subject = decoded.get_subject();
        } else {
            result.missing_claims.emplace_back("sub");
        }
        if (decoded.has_audience()) {
            const auto aud = decoded.get_audience();
            result.audiences.assign(aud.begin(), aud.end());
            if (!aud.empty()) {
                result.audience = *aud.begin();
            }
        }
        if (result.audiences.empty()) {
            result.missing_claims.emplace_back("aud");
        }
        if (decoded.has_payload_claim("scope")) {
            result.scope = decoded.get_payload_claim("scope").as_string();
        } else {
            result.missing_claims.emplace_back("scope");
        }
        if (!decoded.has_expires_at()) {
            result.missing_claims.emplace_back("exp");
        }
    } catch (const std::exception& e) {
        result.valid = false;
        result.error = e.what();
    }
    return result;
}

} // namespace sbi_core::jwt
