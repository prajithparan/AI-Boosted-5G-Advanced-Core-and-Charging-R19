#include "nas_alg_provider/loader.hpp"

#include <openssl/evp.h>

#include <dlfcn.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace nas_alg_provider {

std::string sha256_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "";
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    char buf[4096];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, static_cast<std::size_t>(in.gcount()));
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_DigestFinal_ex(ctx, md, &n);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < n; ++i) {
        out.push_back(hex[md[i] >> 4]);
        out.push_back(hex[md[i] & 0xf]);
    }
    return out;
}

Provider::~Provider() {
    if (handle_ != nullptr) {
        dlclose(handle_);
    }
}

int Provider::eea1(const std::uint8_t key[16],
                   std::uint32_t count,
                   std::uint8_t bearer,
                   std::uint8_t direction,
                   const std::uint8_t* in,
                   std::uint8_t* out,
                   std::uint32_t length_bits) const {
    if ((info_.algorithms & NASALG_ALG_128_EEA1) == 0) {
        return NASALG_E_UNSUPPORTED;
    }
    return eea1_(key, count, bearer, direction, in, out, length_bits);
}

int Provider::eia1(const std::uint8_t key[16],
                   std::uint32_t count,
                   std::uint32_t fresh,
                   std::uint8_t direction,
                   const std::uint8_t* msg,
                   std::uint32_t length_bits,
                   std::uint8_t mac[4]) const {
    if ((info_.algorithms & NASALG_ALG_128_EIA1) == 0) {
        return NASALG_E_UNSUPPORTED;
    }
    return eia1_(key, count, fresh, direction, msg, length_bits, mac);
}

tl::expected<std::unique_ptr<Provider>, std::string> load(const ProviderConfig& cfg,
                                                          const LoadPolicy& policy) {
    using Err = tl::unexpected<std::string>;
    if (cfg.path.empty() || cfg.path.front() != '/') {
        return Err("provider path must be absolute");
    }
    struct stat st {};
    if (::stat(cfg.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return Err("provider file not found");
    }
    if (policy.require_owner_check) {
        if (st.st_uid != 0 && st.st_uid != ::getuid()) {
            return Err("provider file owner is neither root nor the service user");
        }
        if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            return Err("provider file is group- or world-writable");
        }
    }
    if (cfg.sha256.empty() || sha256_file(cfg.path) != cfg.sha256) {
        return Err("provider sha256 does not match the pinned value");
    }
    void* h = ::dlopen(cfg.path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        return Err("dlopen failed");
    }
    std::unique_ptr<Provider> p(new Provider());
    p->handle_ = h; // closed by the destructor on any later failure
    auto describe = reinterpret_cast<decltype(&nasalg_describe)>(::dlsym(h, "nasalg_describe"));
    p->eea1_ = reinterpret_cast<decltype(&nasalg_eea1)>(::dlsym(h, "nasalg_eea1"));
    p->eia1_ = reinterpret_cast<decltype(&nasalg_eia1)>(::dlsym(h, "nasalg_eia1"));
    auto selftest = reinterpret_cast<decltype(&nasalg_selftest)>(::dlsym(h, "nasalg_selftest"));
    if (describe == nullptr || p->eea1_ == nullptr || p->eia1_ == nullptr || selftest == nullptr) {
        return Err("provider is missing a mandatory export");
    }
    const nasalg_info* info = describe();
    if (info == nullptr || info->abi_version != NASALG_ABI_VERSION) {
        return Err("provider ABI version mismatch");
    }
    if ((info->algorithms & (NASALG_ALG_128_EEA1 | NASALG_ALG_128_EIA1)) == 0) {
        return Err("provider offers no algorithm");
    }
    if (info->test_only != 0 && !policy.allow_test_only) {
        return Err("provider is marked test-only");
    }
    p->info_ = *info;
    p->provider_id_ = info->provider_id != nullptr ? info->provider_id : "";
    p->package_id_ = info->package_id != nullptr ? info->package_id : "";
    std::uint32_t passed = 0, total = 0;
    if (selftest(cfg.vectors.c_str(), &passed, &total) != 0) {
        return Err("provider self-test could not run");
    }
    if (total == 0 || passed != total) {
        return Err("provider known-answer self-test failed (" + std::to_string(passed) + "/" +
                   std::to_string(total) + ")");
    }
    p->passed_ = passed;
    return p;
}

} // namespace nas_alg_provider
