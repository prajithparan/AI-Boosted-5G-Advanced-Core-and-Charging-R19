// The content-of-communication chain end to end (ADR-0464), minus real packet capture:
//
//   X1 trigger --LI_T3 (X1, F-SEID target)--> UPF CC-POI --LI_X3--> MDF3 --LI_HI3--> LEMF
//
// The UPF's POI is the real upf::UpfLiPoi in this process (the UPF binary links the same code); the
// MDF3 is the real li-mdf3 process, provisioned over real X1 with the XID -> LIID mapping; the LEMF
// is a loopback TLS listener. The one thing a test must supply is the packet: nothing in the UPF
// datapath calls UpfLiPoi::on_packet yet (the eBPF/XDP program cannot attach without privileges),
// so the tests call the seam directly with GTP-U packets. Needs Valkey; skipped when absent.

#include "sbi_core/http2_client.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "li_core/hi2.hpp"
#include "li_core/x1.hpp"
#include "li_poi.hpp" // nfs/upf/src
#include "li_poi/x1_trigger.hpp"
#include "loopback_lemf.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
namespace x1 = li_core::x1;
namespace hi2 = li_core::hi2;

constexpr const char* kMdf3X1Url = "https://127.0.0.1:7811/X1/NE";
constexpr std::uint16_t kMdf3X3Port = 7812;
constexpr std::uint16_t kUpfX1Port = 19843;
constexpr const char* kUpfX1Url = "https://127.0.0.1:19843/X1/NE";
constexpr const char* kXid = "b0b1b2b3-b4b5-44b7-88b9-babbbcbdbebf";
constexpr const char* kDid = "33333333-3333-4333-8333-333333333333";
constexpr const char* kLiid = "LIID-CC-0001";
constexpr std::uint64_t kCpSeid = 77;
constexpr std::uint32_t kTeid = 0x01020304;

sbi_core::http2::TlsConfig client_tls() {
    return {.cert_path = CERTS_DIR "/hello-nf/cert.pem",
            .key_path = CERTS_DIR "/hello-nf/key.pem",
            .ca_path = CERTS_DIR "/ca/ca.crt"};
}

tl::expected<sbi_core::http2::ClientResponse, std::string> post_x1(sbi_core::http2::Client& client,
                                                                   const std::string& xml) {
    sbi_core::http2::ClientRequest request;
    request.method = "POST";
    request.url = kMdf3X1Url;
    request.headers.emplace("content-type", "application/xml");
    request.body = xml;
    return client.send(request);
}

std::string request_of(x1::MessageType type, x1::RequestBody body, const char* txn) {
    x1::Request r;
    r.header = {"admf-test", "mdf3-01", "2026-10-06T00:00:00.000000Z", "v1.23.1", txn};
    r.type = type;
    r.body = std::move(body);
    const auto xml = x1::serialise_request({r});
    return xml.has_value() ? *xml : std::string();
}

std::uint16_t valkey_port() {
    const char* url = std::getenv("LI_MDF3_REDIS_URL");
    if (url == nullptr) {
        url = std::getenv("LI_MDF_REDIS_URL");
    }
    if (url == nullptr) {
        return 6379;
    }
    const std::string text(url);
    const auto colon = text.rfind(':');
    const auto port =
        colon == std::string::npos ? 0UL : std::strtoul(text.c_str() + colon + 1, nullptr, 10);
    return port > 0 && port <= 65535 ? static_cast<std::uint16_t>(port) : 6379;
}

bool valkey_available() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(valkey_port());
    const bool up = fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    if (fd >= 0) {
        ::close(fd);
    }
    return up;
}

// A GTP-U G-PDU (TS 29.281) with a PDU Session Container extension header carrying `qfi`.
std::vector<std::uint8_t>
gtpu(std::uint32_t teid, const std::vector<std::uint8_t>& inner, std::uint8_t qfi) {
    std::vector<std::uint8_t> out{0x34, 0xFF, 0, 0};
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(teid >> shift));
    }
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x85, 0x01, 0x00, qfi, 0x00});
    out.insert(out.end(), inner.begin(), inner.end());
    const std::size_t length = out.size() - 8;
    out[2] = static_cast<std::uint8_t>(length >> 8);
    out[3] = static_cast<std::uint8_t>(length & 0xFF);
    return out;
}

std::vector<std::uint8_t> ipv4_packet(std::uint8_t last_octet) {
    return {0x45, 0, 0, 20, 0, 0, 0, 0, 64, 17, 0, 0, 10, 0, 0, last_octet, 8, 8, 8, 8};
}

li_poi::Config upf_poi_config() {
    li_poi::Config cfg;
    cfg.x1_bind_address = "127.0.0.1";
    cfg.x1_port = kUpfX1Port;
    cfg.ne_identifier = "upf-poi-test";
    cfg.network_function_id = "upf-01.5gc.example.net";
    cfg.interception_point_id = "UPF-CC-POI";
    cfg.mdf2_host = "127.0.0.1"; // the runtime's one delivery endpoint: here, the MDF3's X3
    cfg.mdf2_port = kMdf3X3Port;
    cfg.mdf2_sni = "localhost";
    cfg.cert_path = CERTS_DIR "/upf/cert.pem";
    cfg.key_path = CERTS_DIR "/upf/key.pem";
    cfg.ca_path = CERTS_DIR "/ca/ca.crt";
    return cfg;
}

} // namespace

TEST(LiCcChain, AnUplinkAndDownlinkPacketOfATargetSessionReachTheLemfAsHi3Cc) {
    if (!valkey_available()) {
        GTEST_SKIP() << "Valkey is not running on 127.0.0.1:" << valkey_port();
    }
    nf_test::LoopbackLemf lemf;
    nf_test::SpawnedProcess mdf3(LI_MDF3_PATH);
    auto admf = sbi_core::http2::Client(client_tls());
    for (int i = 0; i < 100 && !post_x1(admf, "<nonsense/>").has_value(); ++i) {
        std::this_thread::sleep_for(200ms);
    }
    (void)post_x1(admf,
                  request_of(x1::MessageType::DeactivateAllTasks,
                             x1::DeactivateAllTasks{},
                             "00000000-0000-4000-8000-0000000000d1"));
    (void)post_x1(admf,
                  request_of(x1::MessageType::RemoveAllDestinations,
                             x1::RemoveAllDestinations{},
                             "00000000-0000-4000-8000-0000000000d2"));

    // 1. The ADMF provisions the MDF3: where HI3 goes, and the XID -> LIID mapping.
    x1::DestinationDetails destination;
    destination.did = kDid;
    destination.delivery = x1::DeliveryType::X3Only;
    destination.address = {x1::DeliveryAddress::Kind::IpAddressAndPort,
                           "127.0.0.1:" + std::to_string(lemf.port())};
    const auto created = post_x1(admf,
                                 request_of(x1::MessageType::CreateDestination,
                                            x1::CreateDestination{destination},
                                            "00000000-0000-4000-8000-0000000000d3"));
    ASSERT_TRUE(created.has_value()) << created.error();
    ASSERT_EQ(created->body.find("ErrorResponse"), std::string::npos) << created->body;
    x1::TaskDetails mdf_task;
    mdf_task.xid = kXid;
    mdf_task.targets.push_back({x1::TargetIdentifierKind::SupiImsi, "supiimsi", "204081234567890"});
    mdf_task.delivery = x1::DeliveryType::X3Only;
    mdf_task.dids = {kDid};
    x1::MediationDetails mediation;
    mediation.liid = kLiid;
    mediation.delivery = x1::MediationDeliveryType::Hi3Only;
    mdf_task.mediation_details.push_back(mediation);
    const auto activated = post_x1(admf,
                                   request_of(x1::MessageType::ActivateTask,
                                              x1::ActivateTask{mdf_task},
                                              "00000000-0000-4000-8000-0000000000d4"));
    ASSERT_TRUE(activated.has_value()) << activated.error();
    ASSERT_EQ(activated->body.find("ErrorResponse"), std::string::npos) << activated->body;

    // 2. The UPF's CC-POI, triggered the way the SMF's CC-TF does it (LI_T3).
    upf::UpfLiPoi poi(upf_poi_config());
    poi.start();
    li_poi::X1Trigger tf({"smf-cc-tf-test", "upf-poi-test", kUpfX1Url}, client_tls());
    x1::TargetIdentifier fseid;
    fseid.kind = x1::TargetIdentifierKind::UpfFseid;
    fseid.element = "FSEID";
    fseid.value = std::to_string(kCpSeid);
    fseid.address = "127.0.0.1";
    for (int i = 0; i < 100; ++i) {
        if (tf.activate(kXid, {fseid}, {kDid}).has_value()) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    poi.on_session_established(kCpSeid, "127.0.0.1", /*up_seid=*/1000, kTeid);

    // 3. A target-session packet each way, and one on another tunnel.
    const auto up = ipv4_packet(1);
    const auto down = ipv4_packet(2);
    poi.on_packet(0x0BADF00D, upf::PacketDirection::Uplink, gtpu(0x0BADF00D, ipv4_packet(9), 5));
    poi.on_packet(kTeid, upf::PacketDirection::Uplink, gtpu(kTeid, up, 9));
    poi.on_packet(kTeid, upf::PacketDirection::Downlink, gtpu(kTeid, down, 9));
    ASSERT_TRUE(lemf.wait_for_iri(20s)) << "no LI_HI3 record reached the LEMF";
    for (int i = 0; i < 100 && lemf.iri().size() < 2; ++i) {
        std::this_thread::sleep_for(100ms);
    }
    std::this_thread::sleep_for(500ms); // a stray third record would have arrived by now
    const auto received = lemf.iri();
    ASSERT_EQ(received.size(), 2U) << "expected exactly the two packets of the target's tunnel";

    const auto first = hi2::decode_cc_message(received[0]);
    const auto second = hi2::decode_cc_message(received[1]);
    ASSERT_TRUE(first.has_value()) << first.error();
    ASSERT_TRUE(second.has_value()) << second.error();
    EXPECT_EQ(first->header.liid, kLiid); // the ADMF's LIID, not the XID
    EXPECT_EQ(first->header.network_function_identifier, "upf-01.5gc.example.net");
    EXPECT_EQ(first->header.extended_interception_point_id, "UPF-CC-POI");
    EXPECT_EQ(first->payload_direction, std::optional<std::uint8_t>(0));  // fromTarget
    EXPECT_EQ(second->payload_direction, std::optional<std::uint8_t>(1)); // toTarget
    EXPECT_EQ(second->header.sequence_number, first->header.sequence_number + 1);
    const auto c1 = hi2::decode_cc_payload(first->cc_payload);
    const auto c2 = hi2::decode_cc_payload(second->cc_payload);
    ASSERT_TRUE(c1.has_value() && c2.has_value());
    EXPECT_EQ(c1->kind, hi2::CcContentKind::Ip);
    EXPECT_EQ(c1->qfi, std::optional<std::uint8_t>(9));
    EXPECT_EQ(c1->data, up);
    EXPECT_EQ(c2->data, down);

    // 4. The warrant is withdrawn at the UPF: the next packet is not intercepted.
    ASSERT_TRUE(tf.deactivate(kXid).has_value());
    poi.on_packet(kTeid, upf::PacketDirection::Uplink, gtpu(kTeid, ipv4_packet(3), 9));
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(lemf.iri().size(), 2U) << "a packet was intercepted after the task was deactivated";

    // 5. A deleted session is not intercepted either (re-activate, delete session, packet).
    ASSERT_TRUE(tf.activate(kXid, {fseid}, {kDid}).has_value());
    poi.on_session_deleted(1000);
    poi.on_packet(kTeid, upf::PacketDirection::Uplink, gtpu(kTeid, ipv4_packet(4), 9));
    std::this_thread::sleep_for(1s);
    EXPECT_EQ(lemf.iri().size(), 2U) << "a packet of a deleted session was intercepted";
    poi.stop();
}

TEST(LiCcChain, TheUpfPoiRefusesTargetKindsItCannotMatch) {
    upf::UpfLiPoi poi(upf_poi_config());
    poi.start();
    li_poi::X1Trigger tf({"smf-cc-tf-test", "upf-poi-test", kUpfX1Url}, client_tls());
    x1::TargetIdentifier ue_ip;
    ue_ip.kind = x1::TargetIdentifierKind::Ipv4Address;
    ue_ip.element = "ipv4Address";
    ue_ip.value = "10.45.0.2";
    tl::expected<void, std::string> result = tl::make_unexpected(std::string("not sent"));
    for (int i = 0; i < 100; ++i) {
        result = tf.activate("c0c1c2c3-c4c5-44c7-88c9-cacbcccdcecf", {ue_ip}, {kDid});
        // "X1 error" means the listener answered; anything else is a transport failure: retry.
        if (result.has_value() || result.error().find("X1 error") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("3010"), std::string::npos) << result.error();
    poi.stop();
}
