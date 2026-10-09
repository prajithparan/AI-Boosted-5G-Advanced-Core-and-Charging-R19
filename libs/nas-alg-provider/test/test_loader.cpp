// ADR-0479: fail-closed gates of the NAS algorithm provider loader, using a TEST-ONLY dummy
// provider that is NOT SNOW 3G. No conformance of any real algorithm is claimed or tested here.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

#include "nas_alg_provider/loader.hpp"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace nas_alg_provider;

namespace {

struct Fixture {
    fs::path dir = fs::temp_directory_path() / ("nasalg-test-" + std::to_string(::getpid()));
    Fixture() { fs::create_directories(dir); }
    ~Fixture() { fs::remove_all(dir); }
    std::string vectors(const std::string& content) const {
        const auto p = dir / "vectors.txt";
        std::ofstream(p) << content;
        return p.string();
    }
    std::string copy_provider(mode_t mode = 0755) const {
        const auto p = dir / "libprov.so";
        fs::copy_file(TEST_PROVIDER_PATH, p, fs::copy_options::overwrite_existing);
        ::chmod(p.c_str(), mode);
        return p.string();
    }
};

ProviderConfig cfg_for(const std::string& so, const std::string& vec) {
    return ProviderConfig{so, sha256_file(so), vec};
}
const LoadPolicy kAllowTest{.allow_test_only = true};

} // namespace

TEST(NasAlgProvider, Sha256MatchesTheKnownAnswerForAbc) {
    Fixture f;
    const auto p = f.dir / "abc.txt";
    std::ofstream(p, std::ios::binary) << "abc";
    EXPECT_EQ(sha256_file(p.string()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_file((f.dir / "missing").string()), "");
}

TEST(NasAlgProvider, TestOnlyProviderIsRefusedByDefault) {
    Fixture f;
    auto r = load(cfg_for(f.copy_provider(), f.vectors("ok\nok\n")));
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("test-only"), std::string::npos);
}

TEST(NasAlgProvider, LoadsWhenEveryGatePassesAndTheDummyWorks) {
    Fixture f;
    auto r = load(cfg_for(f.copy_provider(), f.vectors("ok\nok\nok\n")), kAllowTest);
    ASSERT_TRUE(r.has_value()) << r.error();
    auto& p = **r;
    EXPECT_EQ(p.selftest_passed(), 3u);
    EXPECT_EQ(p.provider_id(), "TEST-ONLY-NOT-SNOW3G");
    EXPECT_EQ(p.algorithms(), NASALG_ALG_128_EEA1 | NASALG_ALG_128_EIA1);
    const std::uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::uint8_t buf[3] = {0xAA, 0xBB, 0xCC};
    ASSERT_EQ(p.eea1(key, 0, 1, 0, buf, buf, 20), 0); // in-place, 20 bits: low 4 bits zeroed
    EXPECT_EQ(buf[0], 0xAA ^ 1);
    EXPECT_EQ(buf[2] & 0x0F, 0);
    std::uint8_t mac[4];
    EXPECT_EQ(p.eia1(key, 0, 0, 1, buf, 24, mac), 0);
}

TEST(NasAlgProvider, WrongPinnedHashIsRefused) {
    Fixture f;
    auto c = cfg_for(f.copy_provider(), f.vectors("ok\n"));
    c.sha256[0] = c.sha256[0] == 'a' ? 'b' : 'a';
    auto r = load(c, kAllowTest);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("sha256"), std::string::npos);
    c.sha256.clear();
    EXPECT_FALSE(load(c, kAllowTest).has_value()); // an empty pin is not "no pin"
}

TEST(NasAlgProvider, RelativePathMissingFileAndWritableFileAreRefused) {
    Fixture f;
    EXPECT_FALSE(
        load(ProviderConfig{"libprov.so", "x", f.vectors("ok\n")}, kAllowTest).has_value());
    EXPECT_FALSE(
        load(ProviderConfig{(f.dir / "nope.so").string(), "x", f.vectors("ok\n")}, kAllowTest)
            .has_value());
    auto r = load(cfg_for(f.copy_provider(0775), f.vectors("ok\n")), kAllowTest);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("writable"), std::string::npos);
}

TEST(NasAlgProvider, FailedOrEmptySelfTestIsRefused) {
    Fixture f;
    auto bad = load(cfg_for(f.copy_provider(), f.vectors("ok\nbad\n")), kAllowTest);
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().find("1/2"), std::string::npos);
    auto none = load(cfg_for(f.copy_provider(), f.vectors("")), kAllowTest);
    ASSERT_FALSE(none.has_value()); // total == 0 must not pass vacuously
    EXPECT_FALSE(
        load(cfg_for(f.copy_provider(), (f.dir / "absent").string()), kAllowTest).has_value());
}
