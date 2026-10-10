// UPF advertised IPv4 (ADR-0466 follow-up): what the UPF puts in its NRF NFProfile and in its PFCP
// Node ID. Before this the address was a hardcoded 127.0.0.1, which in a container sends the SMF's
// PFCP to the SMF's own loopback. These tests open no listening port: "auto" connect()s a UDP
// socket, which sends nothing, and reads back the local address the kernel chose.

#include "advertised_address.hpp"

#include <gtest/gtest.h>

namespace {

using upf::Ipv4;

TEST(UpfAdvertisedAddress, ALiteralIpv4IsUsedAsIs) {
    const auto r = upf::resolve_advertised_ipv4("10.1.2.3", "https://127.0.0.1:7777");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, (Ipv4{10, 1, 2, 3}));
    EXPECT_EQ(upf::to_string(*r), "10.1.2.3");
}

TEST(UpfAdvertisedAddress, AHostnameIsRejectedBecauseTheSmfParsesALiteral) {
    // SMF does boost::asio::ip::make_address(ip) on what the NRF returns; a name would throw there.
    const auto r = upf::resolve_advertised_ipv4("udsf", "https://127.0.0.1:7777");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("literal IPv4"), std::string::npos) << r.error();
}

TEST(UpfAdvertisedAddress, EmptyOutOfRangeAndIpv6SettingsAreRejected) {
    for (const char* bad : {"", "300.1.1.1", "1.2.3", "::1", "0.0.0.0"}) {
        EXPECT_FALSE(upf::resolve_advertised_ipv4(bad, "https://127.0.0.1:7777").has_value())
            << "accepted: '" << bad << "'";
    }
}

TEST(UpfAdvertisedAddress, AutoTowardALoopbackNrfIsLoopback) {
    const auto r = upf::resolve_advertised_ipv4("auto", "https://127.0.0.1:7777");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, (Ipv4{127, 0, 0, 1}));
}

TEST(UpfAdvertisedAddress, AutoResolvesAHostnameInTheNrfUrl) {
    const auto r = upf::resolve_advertised_ipv4("auto", "https://localhost:7777/some/path");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, (Ipv4{127, 0, 0, 1}));
}

TEST(UpfAdvertisedAddress, AutoWorksWithoutAnExplicitPort) {
    const auto r = upf::resolve_advertised_ipv4("auto", "https://127.0.0.1");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, (Ipv4{127, 0, 0, 1}));
}

TEST(UpfAdvertisedAddress, AutoRefusesAnUnusableNrfUrlInsteadOfFallingBackToLoopback) {
    for (const char* bad :
         {"", "not a url", "https://", "https://:7777", "https://no-such-host.invalid:7777"}) {
        const auto r = upf::resolve_advertised_ipv4("auto", bad);
        EXPECT_FALSE(r.has_value())
            << "accepted NRF url '" << bad << "' -> " << (r ? upf::to_string(*r) : "");
    }
}

TEST(UpfAdvertisedAddress, AutoTowardARoutableNrfPicksTheSourceAddressNotLoopback) {
    // The container case: the NRF is reached over a real interface, so the address to advertise is
    // that interface's, not 127.0.0.1. 192.0.2.1 is TEST-NET-1 (RFC 5737): never routed on the
    // Internet, but a default route still lets the kernel pick a source address. No packet is sent.
    const auto r = upf::resolve_advertised_ipv4("auto", "https://192.0.2.1:7777");
    if (!r.has_value()) {
        GTEST_SKIP() << "no route out of this host, so there is no source address to pick: "
                     << r.error();
    }
    EXPECT_NE((*r)[0], 127) << upf::to_string(*r);
    EXPECT_NE(*r, (Ipv4{0, 0, 0, 0}));
}

} // namespace
