// The shared POI runtime (ADR-0463, libs/li-poi): the part of an IRI-POI that is the same in every
// NF. It is driven the way an ADMF drives it -- real LI_X1 requests over mTLS, built by li_core's
// X1 client codec -- and its output is read from a real LI_X2 server. No NF process is involved;
// the AMF and SMF POIs are tested against it in their own suites.

#include "sbi_core/http2_client.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "li_core/x1.hpp"
#include "li_core/x2x3_pdu.hpp"
#include "li_core/x2x3_server.hpp"
#include "li_poi/poi_runtime.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
namespace x1 = li_core::x1;

constexpr std::uint16_t kX1Port = 19833;
constexpr const char* kX1Url = "https://127.0.0.1:19833/X1/NE";
constexpr const char* kXidA = "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf";
constexpr const char* kXidB = "b0b1b2b3-b4b5-b6b7-b8b9-babbbcbdbebf";

// The X2 Sequence Number attribute (TS 103 221-2 5.3.5): a big-endian u32.
std::optional<std::uint32_t> sequence_of(const li_core::Pdu& pdu) {
    for (const auto& a : pdu.attributes) {
        if (a.type == static_cast<std::uint16_t>(li_core::AttributeType::SequenceNumber) &&
            a.contents.size() == 4) {
            return (std::uint32_t{a.contents[0]} << 24) | (std::uint32_t{a.contents[1]} << 16) |
                   (std::uint32_t{a.contents[2]} << 8) | std::uint32_t{a.contents[3]};
        }
    }
    return std::nullopt;
}

class Collector {
public:
    void operator()(const li_core::Pdu& pdu, std::string_view) {
        const std::lock_guard<std::mutex> lock(mutex_);
        received_.push_back(pdu);
        cv_.notify_all();
    }
    bool wait_for(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return received_.size() >= count; });
    }
    std::vector<li_core::Pdu> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<li_core::Pdu> received_;
};

// What the hook saw: the thread it ran on and the identifiers handed to it.
struct HookLog {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::vector<std::string>>> calls;
    std::vector<std::thread::id> threads;
};

class LiPoiRuntime : public ::testing::Test {
protected:
    void SetUp() override {
        li_core::X2X3ServerConfig sc;
        sc.bind_address = "127.0.0.1";
        sc.port = 0;
        sc.server_cert_path = CERTS_DIR "/amf/cert.pem";
        sc.server_key_path = CERTS_DIR "/amf/key.pem";
        sc.ca_path = CERTS_DIR "/ca/ca.crt";
        mdf2_ = std::make_unique<li_core::X2X3Server>(sc, std::ref(collector_));
        ASSERT_TRUE(mdf2_->start().has_value());

        li_poi::Config cfg;
        cfg.x1_bind_address = "127.0.0.1";
        cfg.x1_port = kX1Port;
        cfg.ne_identifier = "poi-test";
        cfg.network_function_id = "nf.example.net";
        cfg.interception_point_id = "POI-TEST-1";
        cfg.mdf2_host = "127.0.0.1";
        cfg.mdf2_port = mdf2_->bound_port();
        cfg.mdf2_sni = "localhost";
        cfg.cert_path = CERTS_DIR "/amf/cert.pem";
        cfg.key_path = CERTS_DIR "/amf/key.pem";
        cfg.ca_path = CERTS_DIR "/ca/ca.crt";

        li_poi::Hooks hooks;
        hooks.log_name = "poi-test";
        hooks.supported_kinds = {x1::TargetIdentifierKind::SupiImsi,
                                 x1::TargetIdentifierKind::Imsi};
        hooks.normalise = [](const std::string& s) {
            return s.rfind("imsi-", 0) == 0 ? s.substr(5) : s;
        };
        hooks.on_targets_added = [this](const std::string& xid,
                                        const std::vector<x1::TargetIdentifier>& added) {
            const std::lock_guard<std::mutex> lock(hook_.mutex);
            std::vector<std::string> values;
            for (const auto& t : added) {
                values.push_back(t.value);
            }
            hook_.calls.emplace_back(xid, values);
            hook_.threads.push_back(std::this_thread::get_id());
        };
        runtime_ = std::make_unique<li_poi::PoiRuntime>(cfg, hooks);
        runtime_->start();
    }

    void TearDown() override {
        if (runtime_) {
            runtime_->stop();
        }
        if (mdf2_) {
            mdf2_->stop();
        }
    }

    // One X1 request through the client codec over real mTLS; returns the parsed single response.
    x1::ClientResponse send(x1::MessageType type, x1::RequestBody body) {
        x1::Request r;
        r.header = {"admf-1",
                    "poi-test",
                    "2026-10-06T00:00:00.000000Z",
                    "v1.23.1",
                    "2b1e4f6a-0000-4000-8000-0000000000c1"};
        r.type = type;
        r.body = std::move(body);
        const auto xml = x1::serialise_request({r});
        if (!xml) {
            ADD_FAILURE() << xml.error();
            return {};
        }
        sbi_core::http2::TlsConfig tls{
            CERTS_DIR "/hello-nf/cert.pem", CERTS_DIR "/hello-nf/key.pem", CERTS_DIR "/ca/ca.crt"};
        sbi_core::http2::Client client(std::move(tls));
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = kX1Url;
        req.headers.emplace("content-type", "application/xml");
        req.body = *xml;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (const auto reply = client.send(req); reply.has_value()) {
                const auto parsed = x1::parse_response(reply->body);
                EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().detail);
                return parsed.has_value() && !parsed->empty() ? (*parsed)[0] : x1::ClientResponse{};
            }
            std::this_thread::sleep_for(100ms);
        }
        ADD_FAILURE() << "the POI's X1 listener never answered";
        return {};
    }

    static x1::TaskDetails
    task(const std::string& xid,
         std::vector<std::string> imsis,
         x1::TargetIdentifierKind kind = x1::TargetIdentifierKind::SupiImsi) {
        x1::TaskDetails t;
        t.xid = xid;
        for (const auto& i : imsis) {
            t.targets.push_back(
                {kind, kind == x1::TargetIdentifierKind::Imei ? "imei" : "supiimsi", i});
        }
        t.delivery = x1::DeliveryType::X2Only;
        t.dids = {"22222222-2222-4222-8222-222222222222"};
        return t;
    }

    static bool ok(const x1::ClientResponse& r) {
        return std::holds_alternative<x1::OkResponse>(r);
    }
    static std::optional<int> error_of(const x1::ClientResponse& r) {
        const auto* e = std::get_if<x1::ErrorResponse>(&r);
        return e != nullptr ? std::optional(static_cast<int>(e->code)) : std::nullopt;
    }

    std::size_t hook_calls() {
        const std::lock_guard<std::mutex> lock(hook_.mutex);
        return hook_.calls.size();
    }
    bool wait_for_hook(std::size_t n) {
        for (int i = 0; i < 100; ++i) {
            if (hook_calls() >= n) {
                return true;
            }
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }

    Collector collector_;
    HookLog hook_;
    std::unique_ptr<li_core::X2X3Server> mdf2_;
    std::unique_ptr<li_poi::PoiRuntime> runtime_;
};

TEST_F(LiPoiRuntime, TheWarrantStoreFollowsTheX1Lifecycle) {
    EXPECT_TRUE(ok(
        send(x1::MessageType::ActivateTask, x1::ActivateTask{task(kXidA, {"999700000000001"})})));
    // 2010: the same XID again.
    EXPECT_EQ(error_of(send(x1::MessageType::ActivateTask,
                            x1::ActivateTask{task(kXidA, {"999700000000001"})})),
              static_cast<int>(x1::ErrorCode::XidAlreadyExists));
    ASSERT_TRUE(runtime_->task(kXidA).has_value());
    EXPECT_EQ(runtime_->task(kXidA)->targets.size(), 1U);

    // ModifyTask replaces the targets (and creates the task if it is new).
    EXPECT_TRUE(ok(send(x1::MessageType::ModifyTask,
                        x1::ModifyTask{task(kXidA, {"999700000000001", "999700000000002"})})));
    EXPECT_EQ(runtime_->task(kXidA)->targets.size(), 2U);

    EXPECT_TRUE(ok(send(x1::MessageType::DeactivateTask, x1::DeactivateTask{kXidA})));
    EXPECT_FALSE(runtime_->task(kXidA).has_value());
    EXPECT_TRUE(runtime_->matches("999700000000001", {x1::TargetIdentifierKind::SupiImsi}).empty());
}

TEST_F(LiPoiRuntime, ATargetKindThePoiCannotMatchIsRefusedWith3010) {
    // An IMEI cannot be matched by this POI: better refused at provisioning than accepted and
    // silent.
    EXPECT_EQ(error_of(send(x1::MessageType::ActivateTask,
                            x1::ActivateTask{
                                task(kXidA, {"49015420323751"}, x1::TargetIdentifierKind::Imei)})),
              static_cast<int>(x1::ErrorCode::UnsupportedTargetIdentifier));
    EXPECT_FALSE(runtime_->task(kXidA).has_value());
    EXPECT_EQ(error_of(send(
                  x1::MessageType::ModifyTask,
                  x1::ModifyTask{task(kXidA, {"49015420323751"}, x1::TargetIdentifierKind::Imei)})),
              static_cast<int>(x1::ErrorCode::UnsupportedTargetIdentifier));
}

TEST_F(LiPoiRuntime, MatchingIsByKindAndNormalisedIdentityOneMatchPerTask) {
    x1::TaskDetails gated = task(kXidA, {"999700000000001"});
    gated.identifier_association_events = x1::IdentifierAssociationEventsGenerated::All;
    ASSERT_TRUE(ok(send(x1::MessageType::ActivateTask, x1::ActivateTask{gated})));
    ASSERT_TRUE(ok(
        send(x1::MessageType::ActivateTask, x1::ActivateTask{task(kXidB, {"999700000000001"})})));

    // The SBI form "imsi-<digits>" and the bare digits are the same identity (the normaliser).
    const auto m = runtime_->matches("imsi-999700000000001", {x1::TargetIdentifierKind::SupiImsi});
    ASSERT_EQ(m.size(), 2U)
        << "two warrants on one subscriber are each reported under their own XID";
    for (const auto& match : m) {
        if (match.xid == kXidA) {
            EXPECT_EQ(match.identifier_association, x1::IdentifierAssociationEventsGenerated::All);
        } else {
            EXPECT_FALSE(match.identifier_association.has_value());
        }
    }
    // A different identity, or a kind the caller does not ask about, matches nothing.
    EXPECT_TRUE(runtime_->matches("999700000000009", {x1::TargetIdentifierKind::SupiImsi}).empty());
    EXPECT_TRUE(runtime_->matches("999700000000001", {x1::TargetIdentifierKind::Imei}).empty());
}

TEST_F(LiPoiRuntime, TheTargetsAddedHookRunsOffTheX1PathAndOnlyForNewTargets) {
    ASSERT_TRUE(ok(
        send(x1::MessageType::ActivateTask, x1::ActivateTask{task(kXidA, {"999700000000001"})})));
    ASSERT_TRUE(wait_for_hook(1));
    {
        const std::lock_guard<std::mutex> lock(hook_.mutex);
        EXPECT_EQ(hook_.calls[0].first, kXidA);
        EXPECT_EQ(hook_.calls[0].second, (std::vector<std::string>{"999700000000001"}));
        EXPECT_NE(hook_.threads[0], std::this_thread::get_id());
    }
    // A ModifyTask that keeps the old target and adds one: the hook sees ONLY the new one.
    ASSERT_TRUE(ok(send(x1::MessageType::ModifyTask,
                        x1::ModifyTask{task(kXidA, {"999700000000001", "999700000000002"})})));
    ASSERT_TRUE(wait_for_hook(2));
    {
        const std::lock_guard<std::mutex> lock(hook_.mutex);
        EXPECT_EQ(hook_.calls[1].second, (std::vector<std::string>{"999700000000002"}));
    }
    // A ModifyTask that adds nothing does not call it at all.
    ASSERT_TRUE(ok(send(x1::MessageType::ModifyTask,
                        x1::ModifyTask{task(kXidA, {"999700000000001", "999700000000002"})})));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(hook_calls(), 2U);
}

TEST_F(LiPoiRuntime, EmitFramesAnX2PduWithTheXidSequenceAndMatchedTarget) {
    ASSERT_TRUE(ok(
        send(x1::MessageType::ActivateTask, x1::ActivateTask{task(kXidA, {"999700000000001"})})));
    ASSERT_TRUE(ok(
        send(x1::MessageType::ActivateTask, x1::ActivateTask{task(kXidB, {"999700000000001"})})));
    const auto matches = runtime_->matches("999700000000001", {x1::TargetIdentifierKind::SupiImsi});
    ASSERT_EQ(matches.size(), 2U);
    const std::vector<std::uint8_t> payload{0xA1, 0x02, 0x05, 0x00};
    for (int i = 0; i < 3; ++i) {
        for (const auto& m : matches) {
            if (m.xid == kXidA) {
                runtime_->emit(m, payload, li_core::PayloadDirection::FromTarget, "TestRecord");
            }
        }
    }
    for (const auto& m : matches) {
        if (m.xid == kXidB) {
            runtime_->emit(m, payload, li_core::PayloadDirection::NotApplicable, "TestRecord");
        }
    }
    ASSERT_TRUE(collector_.wait_for(4, 10s));
    const auto pdus = collector_.take();
    ASSERT_EQ(pdus.size(), 4U);
    for (const auto& pdu : pdus) {
        EXPECT_EQ(pdu.type, li_core::PduType::X2);
        EXPECT_EQ(pdu.payload, payload) << "the encoded xIRI travels verbatim";
        EXPECT_EQ(li_core::text_attribute(pdu, li_core::AttributeType::MatchedTargetIdentifier),
                  "<supiimsi>999700000000001</supiimsi>");
        EXPECT_EQ(li_core::text_attribute(pdu, li_core::AttributeType::InterceptionPointId),
                  "POI-TEST-1");
    }
    // The sequence number counts per XID (0,1,2 for A; 0 for B), not per connection.
    std::vector<std::uint32_t> a_seq;
    std::vector<std::uint32_t> b_seq;
    const auto a_bytes = li_poi::uuid_to_bytes(kXidA);
    ASSERT_TRUE(a_bytes.has_value());
    for (const auto& pdu : pdus) {
        const auto seq = sequence_of(pdu);
        ASSERT_TRUE(seq.has_value());
        (pdu.xid == *a_bytes ? a_seq : b_seq).push_back(*seq);
    }
    EXPECT_EQ(a_seq, (std::vector<std::uint32_t>{0, 1, 2}));
    EXPECT_EQ(b_seq, (std::vector<std::uint32_t>{0}));
    EXPECT_EQ(pdus.back().payload_direction, li_core::PayloadDirection::NotApplicable);
}

TEST_F(LiPoiRuntime, ReprovisioningAnXidRestartsItsSequenceAndAnUuidThatIsNotOneCannotEmit) {
    ASSERT_TRUE(ok(
        send(x1::MessageType::ActivateTask, x1::ActivateTask{task(kXidA, {"999700000000001"})})));
    auto m = runtime_->matches("999700000000001", {x1::TargetIdentifierKind::SupiImsi}).at(0);
    runtime_->emit(m, std::vector<std::uint8_t>{0x01}, li_core::PayloadDirection::FromTarget, "R");
    ASSERT_TRUE(ok(send(x1::MessageType::DeactivateTask, x1::DeactivateTask{kXidA})));
    ASSERT_TRUE(ok(
        send(x1::MessageType::ActivateTask, x1::ActivateTask{task(kXidA, {"999700000000001"})})));
    m = runtime_->matches("999700000000001", {x1::TargetIdentifierKind::SupiImsi}).at(0);
    runtime_->emit(m, std::vector<std::uint8_t>{0x02}, li_core::PayloadDirection::FromTarget, "R");
    ASSERT_TRUE(collector_.wait_for(2, 10s));
    const auto pdus = collector_.take();
    EXPECT_EQ(sequence_of(pdus[0]), 0U);
    EXPECT_EQ(sequence_of(pdus[1]), 0U);

    // An XID that is not a UUID cannot be framed into the PDU's 128-bit field: nothing is sent.
    li_poi::Match bad{"not-a-uuid", m.target, std::nullopt};
    runtime_->emit(
        bad, std::vector<std::uint8_t>{0x03}, li_core::PayloadDirection::FromTarget, "R");
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(collector_.take().size(), 2U);
}

TEST_F(LiPoiRuntime, UuidToBytesIsStrict) {
    const auto b = li_poi::uuid_to_bytes("a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ((*b)[0], 0xA0);
    EXPECT_EQ((*b)[15], 0xAF);
    EXPECT_FALSE(li_poi::uuid_to_bytes("a0a1a2a3").has_value());
    EXPECT_FALSE(li_poi::uuid_to_bytes("zzzzzzzz-a4a5-a6a7-a8a9-aaabacadaeaf").has_value());
    EXPECT_FALSE(li_poi::uuid_to_bytes("a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf00").has_value());
}

} // namespace
