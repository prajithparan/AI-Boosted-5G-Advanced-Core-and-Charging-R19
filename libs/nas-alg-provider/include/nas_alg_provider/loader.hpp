#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <tl/expected.hpp>
#include <vector>

#include "nas_alg_provider/nas_alg_provider.h"

// Fail-closed loader for an operator-supplied NAS algorithm provider (ADR-0479). Every check
// below must pass or the provider is NOT used: absolute path, owner + permissions, sha256 pin,
// dlopen, all symbols, ABI version, non-empty algorithm set, test_only policy, and a known-answer
// self-test with total > 0 and passed == total. Nothing here knows any algorithm.
namespace nas_alg_provider {

struct ProviderConfig {
    std::string path;    // absolute path of the .so
    std::string sha256;  // lowercase hex of the file, pinned in config
    std::string vectors; // path handed to nasalg_selftest (operator's local vault; never logged)
};

struct LoadPolicy {
    bool allow_test_only = false; // true only in tests
    bool require_owner_check = true;
};

class Provider {
public:
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    std::uint32_t algorithms() const { return info_.algorithms; }
    const std::string& provider_id() const { return provider_id_; }
    const std::string& package_id() const { return package_id_; }
    std::uint32_t selftest_passed() const { return passed_; }

    int eea1(const std::uint8_t key[16],
             std::uint32_t count,
             std::uint8_t bearer,
             std::uint8_t direction,
             const std::uint8_t* in,
             std::uint8_t* out,
             std::uint32_t length_bits) const;
    int eia1(const std::uint8_t key[16],
             std::uint32_t count,
             std::uint32_t fresh,
             std::uint8_t direction,
             const std::uint8_t* msg,
             std::uint32_t length_bits,
             std::uint8_t mac[4]) const;

private:
    friend tl::expected<std::unique_ptr<Provider>, std::string> load(const ProviderConfig&,
                                                                     const LoadPolicy&);
    Provider() = default;
    void* handle_ = nullptr;
    nasalg_info info_{};
    std::string provider_id_, package_id_;
    std::uint32_t passed_ = 0;
    decltype(&nasalg_eea1) eea1_ = nullptr;
    decltype(&nasalg_eia1) eia1_ = nullptr;
};

// The error string never contains vector contents; it names the failed check.
tl::expected<std::unique_ptr<Provider>, std::string> load(const ProviderConfig& cfg,
                                                          const LoadPolicy& policy = {});

// sha256 of a file as lowercase hex ("" if unreadable). Exposed for the importer's lock check.
std::string sha256_file(const std::string& path);

} // namespace nas_alg_provider
