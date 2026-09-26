// UDSF (ADR-0400..ADR-0402): TS 29.598 Nudsf_DataRepository + Nudsf_Timer against a real NRF,
// two real UDSF replicas over TLS 1.3 + mTLS, and real Valkey. Every procedure of TS 29.598
// clause 5.2.2 / 5.3.2 is driven over the wire; response bodies are decoded with the DTOs
// generated from the two YAMLs, so a shape drift fails here.
//
// Isolation on the shared Valkey: the UDSFs are started with a realm unique to this run
// (UDSF_STORAGES), every key lives under udsf:{<realm>/<storage>}:, and the suite deletes exactly
// those keys at the end. Nothing is FLUSHed.
//
// The first block of tests (UdsfLogic.*) needs no processes: the SearchExpression evaluator, the
// JSON Patch helpers, the conditional-request helpers and the sbi-core multipart additions.
#include "sbi_core/datetime.hpp"
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"
#include "sbi_core/multipart.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sw/redis++/redis++.h>
#include <thread>
#include <vector>

#include "TS26510_CommonData_grp.hpp"
#include "TS29598_Nudsf_DataRepository.hpp"
#include "TS29598_Nudsf_Timer.hpp"
#include "http_util.hpp"
#include "patch.hpp"
#include "search.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;
namespace mp = sbi_core::multipart;

// ================================================================================================
// Logic (no processes)
// ================================================================================================

class FakeIndex : public udsf::TagIndex {
public:
    std::map<std::string, std::map<std::string, std::vector<std::string>>> recs;
    std::set<std::string> all() override {
        std::set<std::string> s;
        for (const auto& [id, _] : recs) {
            s.insert(id);
        }
        return s;
    }
    std::set<std::string> eq(const std::string& tag, const std::string& value) override {
        std::set<std::string> s;
        for (const auto& [id, tags] : recs) {
            auto it = tags.find(tag);
            if (it != tags.end() &&
                std::find(it->second.begin(), it->second.end(), value) != it->second.end()) {
                s.insert(id);
            }
        }
        return s;
    }
    std::set<std::string>
    range(const std::string& tag, const std::string& op, const std::string& value) override {
        std::set<std::string> s;
        for (const auto& [id, tags] : recs) {
            auto it = tags.find(tag);
            if (it == tags.end()) {
                continue;
            }
            for (const auto& v : it->second) {
                const bool hit = op == "GT"    ? v > value
                                 : op == "GTE" ? v >= value
                                 : op == "LT"  ? v < value
                                               : v <= value;
                if (hit) {
                    s.insert(id);
                }
            }
        }
        return s;
    }
    std::set<std::string> existing(const std::vector<std::string>& ids) override {
        std::set<std::string> s;
        for (const auto& id : ids) {
            if (recs.count(id) != 0) {
                s.insert(id);
            }
        }
        return s;
    }
    std::optional<std::set<std::string>> with_schema(const std::string&) override {
        return std::nullopt;
    }
};

FakeIndex sample() {
    FakeIndex f;
    f.recs["r1"] = {{"supi", {"imsi-001"}}, {"dnn", {"ims"}}};
    f.recs["r2"] = {{"supi", {"imsi-002"}}, {"dnn", {"internet", "ims"}}};
    f.recs["r3"] = {{"supi", {"imsi-003"}}};
    return f;
}

std::set<std::string> eval(const json& j, FakeIndex& f) {
    auto e = udsf::parse_search_expression(j);
    EXPECT_TRUE(e.has_value()) << (e ? "" : e.error());
    if (!e) {
        return {};
    }
    auto r = udsf::evaluate(*e, f);
    EXPECT_TRUE(r.has_value());
    return r ? *r : std::set<std::string>{};
}

TEST(UdsfLogic, SearchComparisonAndConditions) {
    auto f = sample();
    using S = std::set<std::string>;
    EXPECT_EQ(eval({{"op", "EQ"}, {"tag", "dnn"}, {"value", "ims"}}, f), (S{"r1", "r2"}));
    EXPECT_EQ(eval({{"op", "NEQ"}, {"tag", "dnn"}, {"value", "ims"}}, f), (S{"r3"}));
    EXPECT_EQ(eval({{"op", "GT"}, {"tag", "supi"}, {"value", "imsi-001"}}, f), (S{"r2", "r3"}));
    EXPECT_EQ(eval({{"op", "LTE"}, {"tag", "supi"}, {"value", "imsi-002"}}, f), (S{"r1", "r2"}));
    EXPECT_EQ(eval({{"cond", "OR"},
                    {"units",
                     json::array({{{"op", "EQ"}, {"tag", "supi"}, {"value", "imsi-001"}},
                                  {{"op", "EQ"}, {"tag", "supi"}, {"value", "imsi-003"}}})}},
                   f),
              (S{"r1", "r3"}));
    EXPECT_EQ(eval({{"cond", "AND"},
                    {"units",
                     json::array({{{"op", "EQ"}, {"tag", "dnn"}, {"value", "ims"}},
                                  {{"op", "EQ"}, {"tag", "dnn"}, {"value", "internet"}}})}},
                   f),
              (S{"r2"}));
    EXPECT_EQ(eval({{"cond", "NOT"},
                    {"units", json::array({{{"op", "EQ"}, {"tag", "dnn"}, {"value", "ims"}}})}},
                   f),
              (S{"r3"}));
    EXPECT_EQ(eval({{"recordIdList", json::array({"r2", "nope"})}}, f), (S{"r2"}));
    // 6.1.3.2.3.2: GTE on the empty tag selects everything.
    EXPECT_EQ(eval({{"op", "GTE"}, {"tag", ""}, {"value", ""}}, f), (S{"r1", "r2", "r3"}));
}

TEST(UdsfLogic, SearchCardinalityAndShapeAreEnforced) {
    const json one = {{"op", "EQ"}, {"tag", "a"}, {"value", "b"}};
    EXPECT_FALSE(
        udsf::parse_search_expression({{"cond", "NOT"}, {"units", json::array({one, one})}}));
    EXPECT_FALSE(udsf::parse_search_expression({{"cond", "AND"}, {"units", json::array({one})}}));
    EXPECT_FALSE(
        udsf::parse_search_expression({{"cond", "XOR"}, {"units", json::array({one, one})}}));
    EXPECT_FALSE(udsf::parse_search_expression({{"op", "LIKE"}, {"tag", "a"}, {"value", "b"}}));
    EXPECT_FALSE(udsf::parse_search_expression({{"op", "EQ"}, {"tag", "a"}})); // value missing
    EXPECT_FALSE(udsf::parse_search_expression(
        {{"op", "EQ"}, {"tag", "a"}, {"value", "b"}, {"recordIdList", json::array({"x"})}}));
    EXPECT_FALSE(udsf::parse_filter_param("{not json"));
    // SearchCondition.schemaId on records cannot be honoured (RecordMeta has no schemaId in the
    // R19 YAML) -- an error, never a silently ignored filter.
    auto f = sample();
    auto e = udsf::parse_search_expression(
        {{"cond", "OR"}, {"units", json::array({one, one})}, {"schemaId", "s1"}});
    ASSERT_TRUE(e);
    EXPECT_FALSE(udsf::evaluate(*e, f));
}

TEST(UdsfLogic, JsonPatchItemwiseAndAtomic) {
    auto items = udsf::parse_patch_items(
        R"([{"op":"replace","path":"/a","value":2},{"op":"remove","path":"/missing"},{"op":"add","path":"/b","value":"x"}])");
    ASSERT_TRUE(items);
    auto r =
        udsf::apply_itemwise(json{{"a", 1}}, *items, [](const json&) { return std::string(); });
    EXPECT_EQ(r.document, (json{{"a", 2}, {"b", "x"}}));
    ASSERT_EQ(r.discarded.size(), 1U);
    EXPECT_EQ(r.discarded[0].path, "/missing");
    auto a = udsf::apply_atomic(json{{"a", 1}}, *items);
    ASSERT_FALSE(a);
    EXPECT_EQ(a.error().report.at(0).path, "/missing");
    EXPECT_FALSE(udsf::parse_patch_items("[]"));
}

TEST(UdsfLogic, ConditionalRequestHelpers) {
    const auto tag = udsf::new_etag();
    ASSERT_GE(tag.size(), 3U);
    EXPECT_EQ(tag.front(), '"');
    auto any = udsf::parse_etag_condition(std::string("*"));
    ASSERT_TRUE(any);
    EXPECT_TRUE(udsf::if_match_passes(*any, tag));
    EXPECT_FALSE(udsf::if_match_passes(*any, std::nullopt));
    EXPECT_FALSE(udsf::if_none_match_passes(*any, tag));
    auto list = udsf::parse_etag_condition("\"x\", " + tag);
    EXPECT_TRUE(udsf::if_match_passes(*list, tag));
    auto weak = udsf::parse_etag_condition("W/" + tag);
    EXPECT_FALSE(udsf::if_match_passes(*weak, tag));      // strong comparison
    EXPECT_FALSE(udsf::if_none_match_passes(*weak, tag)); // weak comparison
    EXPECT_EQ(udsf::parse_http_date(udsf::http_date(784111777)), 784111777);
    EXPECT_EQ(udsf::http_date(784111777), "Sun, 06 Nov 1994 08:49:37 GMT");
    const std::string bin("\x00\x01\xff\x10z", 5);
    EXPECT_EQ(udsf::base64_decode(udsf::base64_encode(bin)), bin);
}

TEST(UdsfLogic, MultipartMixedAndParallelRoundTrip) {
    std::vector<mp::Part> parts{
        {"application/json", std::string("meta"), R"({"tags":{"a":["b"]}})"}};
    mp::Part bin{
        "application/octet-stream", std::string("blk1"), std::string("\x00\r\n--x\x01", 6)};
    bin.content_transfer_encoding = "binary";
    parts.push_back(bin);
    parts.push_back({"text/plain", std::string("empty"), ""});
    for (const char* sub : {"mixed", "parallel"}) {
        const auto enc = mp::encode_subtype(sub, parts);
        EXPECT_EQ(enc.content_type_header.rfind(std::string("multipart/") + sub + ";", 0), 0U);
        EXPECT_EQ(enc.content_type_header.find("type="), std::string::npos);
        auto back = mp::parse_any(enc.content_type_header, enc.body);
        ASSERT_TRUE(back) << back.error();
        ASSERT_EQ(back->size(), 3U);
        EXPECT_EQ((*back)[1].body, bin.body);
        EXPECT_EQ((*back)[1].content_transfer_encoding, "binary");
        EXPECT_EQ((*back)[2].body, "");
        EXPECT_EQ((*back)[2].content_id, "empty");
    }
    EXPECT_FALSE(mp::parse("multipart/mixed; boundary=x", "--x\r\n\r\nb\r\n--x--")); // related-only
}

// ================================================================================================
// Over the wire
// ================================================================================================

constexpr const char* kNrf = "https://127.0.0.1:7777";
constexpr const char* kUdsfA = "https://127.0.0.1:7830";
constexpr const char* kUdsfB = "https://127.0.0.1:7831";
constexpr int kReceiverPort = 19931;
constexpr const char* kReceiver = "https://127.0.0.1:19931";

std::string realm() {
    static const std::string r =
        "udsf-it-" + std::to_string(::getpid()) + "-" + std::to_string(std::time(nullptr));
    return r;
}

std::string redis_url() {
    const char* env = std::getenv("UDSF_REDIS_URL");
    return env != nullptr ? env : "tcp://127.0.0.1:6379";
}

sbi_core::http2::Client make_client() {
    return sbi_core::http2::Client(sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    });
}

std::string pct(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

struct Seen {
    std::string path;
    std::multimap<std::string, std::string> headers;
    std::string body;
};

class Receiver {
public:
    Receiver()
        : server_(ioc_,
                  "127.0.0.1",
                  kReceiverPort,
                  sbi_core::http2::TlsConfig{.cert_path = CERTS_DIR "/hello-nf/cert.pem",
                                             .key_path = CERTS_DIR "/hello-nf/key.pem",
                                             .ca_path = CERTS_DIR "/ca/ca.crt"}) {
        for (const char* p : {"/record-expired", "/data-change", "/sub-expiry", "/timer"}) {
            server_.add_route("POST", p, [this, p](const sbi_core::http2::Request& req) {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    seen_.push_back({p, req.headers, req.body});
                }
                cv_.notify_all();
                sbi_core::http2::Response r;
                r.status = 204;
                return r;
            });
        }
        server_.start();
        thread_ = std::thread([this] { ioc_.run(); });
    }
    ~Receiver() {
        ioc_.stop();
        thread_.join();
    }
    // Waits until `n` POSTs arrived on `path`; returns them.
    std::vector<Seen> wait(const std::string& path, std::size_t n, std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(mutex_);
        std::vector<Seen> out;
        cv_.wait_for(lock, limit, [&] {
            out.clear();
            for (const auto& s : seen_) {
                if (s.path == path) {
                    out.push_back(s);
                }
            }
            return out.size() >= n;
        });
        return out;
    }
    void clear() {
        const std::lock_guard<std::mutex> lock(mutex_);
        seen_.clear();
    }

private:
    boost::asio::io_context ioc_;
    sbi_core::http2::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Seen> seen_;
};

std::string header_of(const sbi_core::http2::ClientResponse& r, const std::string& k) {
    auto it = r.headers.find(k);
    return it == r.headers.end() ? "" : it->second;
}

class Udsf : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const std::string storages = json{{realm(), json::array({"S1", "S2"})}}.dump();
        setenv("UDSF_STORAGES", storages.c_str(), 1);
        setenv("UDSF_EXPIRY_SWEEP_INTERVAL_MS", "100", 1);
        setenv("UDSF_MAX_SUBSCRIPTION_SECONDS", "3600", 1);
        nrf_ = std::make_unique<nf_test::SpawnedProcess>(NRF_PATH);
        a_ = std::make_unique<nf_test::SpawnedProcess>(UDSF_PATH);
        setenv("UDSF_PORT", "7831", 1);
        setenv("UDSF_METRICS_BIND_ADDRESS", "127.0.0.1:9531", 1);
        b_ = std::make_unique<nf_test::SpawnedProcess>(UDSF_PATH); // the second replica
        unsetenv("UDSF_PORT");
        unsetenv("UDSF_METRICS_BIND_ADDRESS");
        receiver_ = std::make_unique<Receiver>();
        nf_test::wait_tcp_listening(7777);
        nf_test::wait_tcp_listening(7830);
        nf_test::wait_tcp_listening(7831);
    }
    static void TearDownTestSuite() {
        receiver_.reset();
        b_.reset();
        a_.reset();
        nrf_.reset();
        try {
            sw::redis::Redis r(redis_url());
            for (const char* st : {"S1", "S2"}) {
                long long cursor = 0;
                const std::string pattern = "udsf:{" + realm() + "/" + st + "}:*";
                do {
                    std::vector<std::string> keys;
                    cursor = r.scan(cursor, pattern, 500, std::back_inserter(keys));
                    if (!keys.empty()) {
                        r.del(keys.begin(), keys.end());
                    }
                } while (cursor != 0);
            }
        } catch (const std::exception&) {
        }
    }

    static std::string base(const char* host = kUdsfA) {
        return std::string(host) + "/nudsf-dr/v1/" + realm() + "/S1";
    }
    static std::string tbase() {
        return std::string(kUdsfA) + "/nudsf-timer/v1/" + realm() + "/S1";
    }

    sbi_core::http2::ClientResponse send(const std::string& method,
                                         const std::string& url,
                                         const std::string& body = "",
                                         const std::string& ct = "",
                                         std::multimap<std::string, std::string> hdrs = {}) {
        sbi_core::http2::ClientRequest req;
        req.method = method;
        req.url = url;
        req.body = body;
        req.headers = std::move(hdrs);
        if (!ct.empty()) {
            req.headers.emplace("content-type", ct);
        }
        auto r = client_.send(req);
        EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
        return r ? *r : sbi_core::http2::ClientResponse{};
    }

    sbi_core::http2::ClientResponse put_record(const std::string& id,
                                               const json& meta,
                                               const std::vector<mp::Part>& blocks = {},
                                               std::multimap<std::string, std::string> hdrs = {},
                                               const std::string& q = "") {
        std::vector<mp::Part> parts{{"application/json", std::string("meta"), meta.dump()}};
        parts.insert(parts.end(), blocks.begin(), blocks.end());
        const auto enc = mp::encode_subtype("mixed", parts);
        return send("PUT",
                    base() + "/records/" + id + q,
                    enc.body,
                    enc.content_type_header,
                    std::move(hdrs));
    }

    static std::string cause(const sbi_core::http2::ClientResponse& r) {
        try {
            return json::parse(r.body).value("cause", "");
        } catch (const std::exception&) {
            return "";
        }
    }

    static mp::Part block(const std::string& id, const std::string& ct, const std::string& data) {
        mp::Part p{ct, id, data};
        p.content_transfer_encoding = "binary";
        return p;
    }

    void wait_ready() {
        for (int i = 0; i < 100; ++i) {
            sbi_core::http2::ClientRequest req;
            req.method = "GET";
            req.url = base() + "/records/none";
            if (auto r = client_.send(req); r && r->status == 404) {
                sbi_core::http2::ClientRequest rb = req;
                rb.url = base(kUdsfB) + "/records/none";
                if (auto r2 = client_.send(rb); r2 && r2->status == 404) {
                    return;
                }
            }
            std::this_thread::sleep_for(100ms);
        }
        FAIL() << "UDSF replicas never came up";
    }

    void SetUp() override { wait_ready(); }

    sbi_core::http2::Client client_ = make_client();
    static inline std::unique_ptr<nf_test::SpawnedProcess> nrf_;
    static inline std::unique_ptr<nf_test::SpawnedProcess> a_;
    static inline std::unique_ptr<nf_test::SpawnedProcess> b_;
    static inline std::unique_ptr<Receiver> receiver_;
};

std::string token(sbi_core::http2::Client& c, const std::string& scope, const std::string& nf) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = std::string(kNrf) + "/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=udsf-test&scope=" + scope +
               "&targetNfType=" + nf;
    auto resp = c.send(req);
    if (!resp || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

// 5.2.2.3.2 / 5.2.2.2.2 / 5.2.2.4.2 / 5.2.2.5.2 with conditional requests and get-previous.
TEST_F(Udsf, RecordCreateRetrieveUpdateDelete) {
    const json meta{{"tags", {{"supi", {"imsi-001010000000001"}}, {"dnn", {"ims"}}}}};
    auto r = put_record("rec1",
                        meta,
                        {block("b1", "application/octet-stream", std::string("\x00\x01\x02", 3)),
                         block("b2", "application/json", R"({"k":1})")});
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_NE(header_of(r, "location").find("/nudsf-dr/v1/" + realm() + "/S1/records/rec1"),
              std::string::npos);
    const auto etag1 = header_of(r, "etag");
    ASSERT_FALSE(etag1.empty());
    auto created = mp::parse_any(header_of(r, "content-type"), r.body);
    ASSERT_TRUE(created) << created.error();
    EXPECT_EQ(json::parse((*created)[0].body).get<sbi_gen::RecordMeta>().tags, meta["tags"]);

    // The other replica sees it: no in-process state.
    auto g = send("GET", base(kUdsfB) + "/records/rec1");
    ASSERT_EQ(g.status, 200) << g.body;
    auto parts = mp::parse_any(header_of(g, "content-type"), g.body);
    ASSERT_TRUE(parts) << parts.error();
    ASSERT_EQ(parts->size(), 3U);
    EXPECT_EQ((*parts)[0].content_id, "meta");
    EXPECT_EQ((*parts)[1].content_id, "b1");
    EXPECT_EQ((*parts)[1].body, std::string("\x00\x01\x02", 3));
    EXPECT_EQ((*parts)[1].content_transfer_encoding, "binary");
    EXPECT_EQ((*parts)[2].content_type, "application/json");
    EXPECT_EQ(header_of(g, "etag"), etag1);
    EXPECT_FALSE(header_of(g, "last-modified").empty());
    EXPECT_EQ(header_of(g, "cache-control"), "max-age=60");

    EXPECT_EQ(send("GET", base() + "/records/rec1", "", "", {{"if-none-match", etag1}}).status,
              304);
    auto pre = send("GET", base() + "/records/rec1", "", "", {{"if-match", "\"stale\""}});
    EXPECT_EQ(pre.status, 412);
    EXPECT_EQ(cause(pre), "INCORRECT_CONDITIONAL_GET_REQUEST");
    EXPECT_TRUE(json::parse(pre.body).contains("meta")); // ProblemDetailsExtension

    // If-None-Match: * on an existing record -> 412 with the current ETag.
    auto again = put_record("rec1", meta, {}, {{"if-none-match", "*"}});
    EXPECT_EQ(again.status, 412);
    EXPECT_EQ(header_of(again, "etag"), etag1);

    // Update replaces meta and blocks (6.1.3.3.3.2) and returns the previous record.
    const json meta2{{"tags", {{"supi", {"imsi-001010000000001"}}}}};
    auto u = put_record("rec1", meta2, {}, {{"if-match", etag1}}, "?get-previous=true");
    ASSERT_EQ(u.status, 200) << u.body;
    auto prev = mp::parse_any(header_of(u, "content-type"), u.body);
    ASSERT_TRUE(prev);
    EXPECT_EQ(prev->size(), 3U); // the previous record had two blocks
    const auto etag2 = header_of(u, "etag");
    EXPECT_NE(etag2, etag1);
    EXPECT_EQ(send("GET", base() + "/records/rec1/blocks").status, 204); // blocks discarded

    auto u2 = put_record("rec1", meta2);
    EXPECT_EQ(u2.status, 204);

    auto d412 =
        send("DELETE", base() + "/records/rec1?get-previous=true", "", "", {{"if-match", etag1}});
    EXPECT_EQ(d412.status, 412);
    EXPECT_TRUE(mp::parse_any(header_of(d412, "content-type"), d412.body)); // RecordBody
    auto d = send("DELETE", base() + "/records/rec1?get-previous=true");
    ASSERT_EQ(d.status, 200);
    auto gone = send("GET", base() + "/records/rec1");
    EXPECT_EQ(gone.status, 404);
    EXPECT_EQ(cause(gone), "RECORD_NOT_FOUND");
    EXPECT_EQ(send("DELETE", base() + "/records/rec1").status, 404);
}

TEST_F(Udsf, RealmAndStorageNotFoundAndBadBodies) {
    auto r = send("GET", std::string(kUdsfA) + "/nudsf-dr/v1/NoSuchRealm/S1/records/x");
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(cause(r), "REALM_NOT_FOUND");
    EXPECT_EQ(header_of(r, "content-type"), "application/problem+json");
    r = send("GET", std::string(kUdsfA) + "/nudsf-dr/v1/" + realm() + "/Nope/records/x");
    EXPECT_EQ(cause(r), "STORAGE_NOT_FOUND");
    r = send("GET", std::string(kUdsfA) + "/nudsf-timer/v1/" + realm() + "/Nope/timers/x");
    EXPECT_EQ(cause(r), "STORAGE_NOT_FOUND");
    r = send("PUT", base() + "/records/x", "{}", "application/json");
    EXPECT_EQ(r.status, 415);
    r = put_record("x", json{{"tags", {{"a", json::array()}}}}); // minItems 1
    EXPECT_EQ(r.status, 400);
    r = put_record("x", json{{"tags", {{"a", {"v", "v"}}}}}); // uniqueItems
    EXPECT_EQ(r.status, 400);
}

// 5.2.2.2.3 Meta Retrieval, 5.2.2.4.4 Meta Update (item-wise, 200 PatchResult on discard).
TEST_F(Udsf, MetaRetrievalAndUpdate) {
    ASSERT_EQ(put_record("m1", json{{"tags", {{"ueId", {"455345"}}, {"recordId", {"1"}}}}}).status,
              201);
    auto g = send("GET", base() + "/records/m1/meta");
    ASSERT_EQ(g.status, 200);
    EXPECT_EQ(json::parse(g.body).get<sbi_gen::RecordMeta>().tags,
              (json{{"ueId", {"455345"}}, {"recordId", {"1"}}}));
    auto p = send(
        "PATCH",
        base() + "/records/m1/meta",
        R"([{"op":"replace","path":"/tags/ueId","value":["450005"]},{"op":"remove","path":"/tags/recordId"}])",
        "application/json-patch+json");
    EXPECT_EQ(p.status, 204) << p.body;
    p = send(
        "PATCH",
        base() + "/records/m1/meta",
        R"([{"op":"add","path":"/tags/x","value":["1"]},{"op":"remove","path":"/tags/nope"},{"op":"add","path":"/tags/y","value":[]}])",
        "application/json-patch+json");
    ASSERT_EQ(p.status, 200) << p.body;
    const auto pr = json::parse(p.body).get<sbi_gen::PatchResult>();
    ASSERT_EQ(pr.report.size(), 2U);
    g = send("GET", base() + "/records/m1/meta");
    EXPECT_EQ(json::parse(g.body)["tags"], (json{{"ueId", {"450005"}}, {"x", {"1"}}}));
    EXPECT_EQ(send("PATCH",
                   base() + "/records/nope/meta",
                   R"([{"op":"remove","path":"/tags"}])",
                   "application/json-patch+json")
                  .status,
              404);
}

// 5.2.2.2.4 / 5.2.2.2.5 / 5.2.2.3.3 / 5.2.2.4.3 / 5.2.2.5.3.
TEST_F(Udsf, BlockLifecycle) {
    ASSERT_EQ(put_record("k1", json{{"tags", {{"t", {"1"}}}}}).status, 201);
    auto c = send("PUT",
                  base() + "/records/k1/blocks/blob",
                  std::string("\x10\x20", 2),
                  "application/vnd.test");
    ASSERT_EQ(c.status, 201) << c.body;
    EXPECT_NE(header_of(c, "location").find("/records/k1/blocks/blob"), std::string::npos);
    const auto etag = header_of(c, "etag");
    auto g = send("GET", base() + "/records/k1/blocks/blob");
    ASSERT_EQ(g.status, 200);
    EXPECT_EQ(g.body, std::string("\x10\x20", 2));
    EXPECT_EQ(header_of(g, "content-type"), "application/vnd.test");
    EXPECT_EQ(header_of(g, "etag"), etag);
    // No Content-Type: application/octet-stream (6.1.3.6.3.2).
    sbi_core::http2::ClientRequest raw;
    raw.method = "PUT";
    raw.url = base() + "/records/k1/blocks/raw";
    raw.body = "xyz";
    raw.headers.emplace("content-type", ""); // libcurl: an empty header disables its form default
    ASSERT_EQ(client_.send(raw)->status, 201);
    EXPECT_EQ(header_of(send("GET", base() + "/records/k1/blocks/raw"), "content-type"),
              "application/octet-stream");

    auto list = send("GET", base() + "/records/k1/blocks");
    ASSERT_EQ(list.status, 200);
    EXPECT_EQ(header_of(list, "content-type").rfind("multipart/parallel;", 0), 0U);
    auto parts = mp::parse_any(header_of(list, "content-type"), list.body);
    ASSERT_TRUE(parts);
    EXPECT_EQ(parts->size(), 2U);

    auto u = send("PUT",
                  base() + "/records/k1/blocks/blob?get-previous=true",
                  "new",
                  "text/plain",
                  {{"if-match", etag}});
    ASSERT_EQ(u.status, 200);
    EXPECT_EQ(u.body, std::string("\x10\x20", 2)); // previous value
    EXPECT_EQ(
        send("PUT", base() + "/records/k1/blocks/blob", "z", "text/plain", {{"if-match", etag}})
            .status,
        412);
    EXPECT_EQ(send("PUT", base() + "/records/k1/blocks/blob", "z", "text/plain").status, 204);
    auto d = send("DELETE", base() + "/records/k1/blocks/blob?get-previous=true");
    ASSERT_EQ(d.status, 200);
    EXPECT_EQ(d.body, "z");
    auto nf = send("GET", base() + "/records/k1/blocks/blob");
    EXPECT_EQ(cause(nf), "BLOCK_NOT_FOUND");
    EXPECT_EQ(cause(send("PUT", base() + "/records/nope/blocks/b", "x", "text/plain")),
              "RECORD_NOT_FOUND");
}

// 5.2.2.4.8 Record Partial Update (PartialRecordUpdate, all-or-nothing, 422 PatchResult).
TEST_F(Udsf, RecordPartialUpdate) {
    ASSERT_EQ(
        put_record("p1",
                   json{{"tags", {{"a", {"1"}}}}},
                   {block("old", "text/plain", "old-bytes"), block("keep", "text/plain", "k")})
            .status,
        201);
    const json patch =
        json::array({{{"op", "replace"}, {"path", "/meta/tags/a"}, {"value", {"2"}}},
                     {{"op", "remove"}, {"path", "/blocks/old"}},
                     {{"op", "add"}, {"path", "/blocks/fresh"}, {"value", "part-1"}}});
    std::vector<mp::Part> body{{"application/json-patch+json", std::string("patch"), patch.dump()},
                               block("part-1", "application/octet-stream", "fresh-bytes")};
    auto enc = mp::encode_subtype("mixed", body);
    auto r = send("PATCH", base() + "/records/p1", enc.body, enc.content_type_header);
    ASSERT_EQ(r.status, 204) << r.body;
    auto g = send("GET", base() + "/records/p1");
    auto parts = mp::parse_any(header_of(g, "content-type"), g.body);
    ASSERT_TRUE(parts);
    ASSERT_EQ(parts->size(), 3U);
    EXPECT_EQ(json::parse((*parts)[0].body)["tags"]["a"], json::array({"2"}));
    EXPECT_EQ((*parts)[1].content_id, "fresh");
    EXPECT_EQ((*parts)[1].body, "fresh-bytes");
    EXPECT_EQ((*parts)[2].content_id, "keep");
    // One failing item -> nothing applied, 422 with the report.
    const json bad = json::array({{{"op", "replace"}, {"path", "/meta/tags/a"}, {"value", {"3"}}},
                                  {{"op", "remove"}, {"path", "/blocks/missing"}}});
    enc = mp::encode_subtype("mixed",
                             {{"application/json-patch+json", std::string("patch"), bad.dump()}});
    r = send("PATCH", base() + "/records/p1", enc.body, enc.content_type_header);
    ASSERT_EQ(r.status, 422) << r.body;
    EXPECT_EQ(json::parse(r.body).get<sbi_gen::PatchResult>().report.at(0).path, "/blocks/missing");
    g = send("GET", base() + "/records/p1/meta");
    EXPECT_EQ(json::parse(g.body)["tags"]["a"], json::array({"2"}));
}

// 5.2.2.2.6 Search (+ CombinedSearchRetrieve, BulkOperations) and 5.2.2.5.5 Bulk Records Delete.
TEST_F(Udsf, SearchAndBulkDelete) {
    const std::string b2 = std::string(kUdsfA) + "/nudsf-dr/v1/" + realm() + "/S2";
    auto put2 = [&](const std::string& id, const json& tags, const std::string& blob) {
        const auto enc = mp::encode_subtype(
            "mixed",
            {{"application/json", std::string("meta"), json{{"tags", tags}}.dump()},
             block("b", "text/plain", blob)});
        return send("PUT", b2 + "/records/" + id, enc.body, enc.content_type_header).status;
    };
    ASSERT_EQ(put2("s1", {{"supi", {"imsi-1"}}, {"dnn", {"ims"}}}, "one"), 201);
    ASSERT_EQ(put2("s2", {{"supi", {"imsi-2"}}, {"dnn", {"ims", "internet"}}}, "two"), 201);
    ASSERT_EQ(put2("s3", {{"supi", {"imsi-3"}}}, "three"), 201);
    auto search = [&](const json& filter, const std::string& extra = "") {
        return send("GET", b2 + "/records?filter=" + pct(filter.dump()) + extra);
    };
    auto r = search({{"op", "EQ"}, {"tag", "dnn"}, {"value", "ims"}});
    ASSERT_EQ(r.status, 200) << r.body;
    auto d = json::parse(r.body).get<sbi_gen::RecordSearchResultDescriptor>();
    EXPECT_EQ(d.count, 2);
    ASSERT_TRUE(d.references);
    EXPECT_EQ(d.references->size(), 2U);
    r = search({{"cond", "NOT"},
                {"units", json::array({{{"op", "EQ"}, {"tag", "dnn"}, {"value", "ims"}}})}});
    EXPECT_EQ(json::parse(r.body)["count"], 1);
    r = search({{"op", "GT"}, {"tag", "supi"}, {"value", "imsi-1"}}, "&limit-range=1");
    d = json::parse(r.body).get<sbi_gen::RecordSearchResultDescriptor>();
    EXPECT_EQ(d.count, 2);
    EXPECT_EQ(d.references->size(), 1U);
    r = search({{"op", "GTE"}, {"tag", ""}, {"value", ""}}, "&count-indicator=true");
    d = json::parse(r.body).get<sbi_gen::RecordSearchResultDescriptor>();
    EXPECT_EQ(d.count, 3);
    EXPECT_FALSE(d.references);
    EXPECT_EQ(search({{"op", "EQ"}, {"tag", "dnn"}, {"value", "none"}}).status, 204);
    EXPECT_EQ(search({{"cond", "AND"},
                      {"units", json::array({{{"op", "EQ"}, {"tag", "a"}, {"value", "b"}}})}})
                  .status,
              400);
    EXPECT_EQ(send("GET", b2 + "/records").status, 400);
    EXPECT_EQ(send("GET", b2 + "/records?tag-count-filter=" + pct("{}")).status, 400);
    // CombinedSearchRetrieve: descriptor part first, then <id>/meta and <id>/<blockId>.
    r = search({{"recordIdList", json::array({"s1", "s3", "gone"})}},
               "&retrieve-records=META_AND_BLOCKS&supported-features=6D");
    ASSERT_EQ(r.status, 200);
    auto parts = mp::parse_any(header_of(r, "content-type"), r.body);
    ASSERT_TRUE(parts) << parts.error();
    ASSERT_EQ(parts->size(), 5U);
    EXPECT_EQ((*parts)[0].content_id, "recordSearchResultDescriptor");
    const auto desc = json::parse((*parts)[0].body).get<sbi_gen::RecordSearchResultDescriptor>();
    EXPECT_EQ(desc.count, 2);
    EXPECT_EQ(desc.supportedFeatures, "6D");
    EXPECT_EQ((*parts)[1].content_id, "s1/meta");
    EXPECT_EQ((*parts)[2].content_id, "s1/b");
    EXPECT_EQ((*parts)[2].body, "one");
    // max-payload-size caps the records included: 0 KB admits none, the descriptor still comes.
    r = search({{"op", "GTE"}, {"tag", ""}, {"value", ""}},
               "&retrieve-records=ONLY_META&max-payload-size=0");
    parts = mp::parse_any(header_of(r, "content-type"), r.body);
    ASSERT_TRUE(parts);
    EXPECT_EQ(parts->size(), 1U);

    // Bulk delete by filter, then everything (GTE ""), then nothing left -> 204.
    r = send("DELETE",
             b2 + "/records?filter=" +
                 pct(json{{"op", "EQ"}, {"tag", "supi"}, {"value", "imsi-1"}}.dump()));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).get<sbi_gen::RecordDeleteResponse>().recordIdList,
              std::vector<std::string>{"s1"});
    r = send("DELETE",
             b2 + "/records?filter=" + pct(json{{"op", "GTE"}, {"tag", ""}, {"value", ""}}.dump()));
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body)["recordIdList"].size(), 2U);
    EXPECT_EQ(
        send("DELETE",
             b2 + "/records?filter=" + pct(json{{"op", "GTE"}, {"tag", ""}, {"value", ""}}.dump()))
            .status,
        204);
    EXPECT_EQ(send("DELETE", b2 + "/records").status, 400);
}

// 5.2.2.7.2 Subscribe, 5.2.2.2.7/8 retrieval, 5.2.2.4.5/6 update, 5.2.2.6.3 data-change
// notification, 5.2.2.8.2 unsubscribe.
TEST_F(Udsf, SubscriptionsAndDataChangeNotification) {
    receiver_->clear();
    ASSERT_EQ(put_record("w1", json{{"tags", {{"t", {"1"}}}}}).status, 201);
    const std::string monitored = "/nudsf-dr/v1/" + realm() + "/S1/records/w1";
    const json sub{{"clientId", {{"nfId", "11111111-1111-4111-8111-111111111111"}}},
                   {"callbackReference", std::string(kReceiver) + "/data-change"},
                   {"subFilter", {{"monitoredResourceUris", {monitored}}}},
                   {"supportedFeatures", "1"}};
    auto r = send("PUT", base() + "/subs-to-notify/sub1", sub.dump(), "application/json");
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_FALSE(header_of(r, "location").empty());
    auto created = json::parse(r.body).get<sbi_gen::NotificationSubscription>();
    EXPECT_TRUE(
        created.expiry.has_value()); // operator ceiling applied (UDSF_MAX_SUBSCRIPTION_SECONDS)
    EXPECT_EQ(created.supportedFeatures, "1");

    // Missing monitored record -> 409 with the missing URIs.
    json bad = sub;
    bad["subFilter"]["monitoredResourceUris"] = {monitored + "-missing"};
    r = send("PUT", base() + "/subs-to-notify/sub2", bad.dump(), "application/json");
    ASSERT_EQ(r.status, 409);
    EXPECT_EQ(json::parse(r.body), json::array({monitored + "-missing"}));
    // Another client may not take it over.
    json other = sub;
    other["clientId"] = {{"nfSetId", "set-x"}};
    EXPECT_EQ(cause(send("PUT", base() + "/subs-to-notify/sub1", other.dump(), "application/json")),
              "SUBSCRIPTION_EXISTS");

    EXPECT_EQ(send("GET", base() + "/subs-to-notify/sub1").status, 200);
    r = send("GET", base() + "/subs-to-notify?limit-range=5");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body).get<std::vector<sbi_gen::NotificationSubscription>>().size(), 1U);

    // A change to the monitored record reaches the callback (via the other replica, too).
    const auto enc = mp::encode_subtype(
        "mixed",
        {{"application/json", std::string("meta"), json{{"tags", {{"t", {"2"}}}}}.dump()},
         block("x", "text/plain", "payload")});
    ASSERT_EQ(send("PUT", base(kUdsfB) + "/records/w1", enc.body, enc.content_type_header).status,
              204);
    auto got = receiver_->wait("/data-change", 1, 10s);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].headers.find("3gpp-sbi-callback")->second,
              "Nudsf_DataRepository_onDataChange");
    auto parts = mp::parse_any(got[0].headers.find("content-type")->second, got[0].body);
    ASSERT_TRUE(parts) << parts.error();
    ASSERT_EQ(parts->size(), 3U);
    const auto desc = json::parse((*parts)[0].body).get<sbi_gen::NotificationDescription>();
    EXPECT_EQ(desc.operationType.value, "UPDATED");
    EXPECT_EQ(desc.subscriptionId, "sub1");
    EXPECT_NE(desc.recordRef.find("/records/w1"), std::string::npos);
    EXPECT_EQ((*parts)[2].body, "payload");

    // Filter to DELETED only via PATCH; an update no longer notifies, a delete does.
    r = send("PATCH",
             base() + "/subs-to-notify/sub1",
             R"([{"op":"add","path":"/subFilter/operations","value":["DELETED"]}])",
             "application/json-patch+json");
    EXPECT_EQ(r.status, 204) << r.body;
    r = send("PATCH",
             base() + "/subs-to-notify/sub1",
             R"([{"op":"replace","path":"/clientId","value":{"nfSetId":"x"}}])",
             "application/json-patch+json");
    EXPECT_EQ(r.status, 200); // discarded: the owner cannot change
    receiver_->clear();
    ASSERT_EQ(put_record("w1", json{{"tags", {{"t", {"3"}}}}}).status, 204);
    ASSERT_EQ(send("DELETE", base() + "/records/w1").status, 204);
    got = receiver_->wait("/data-change", 1, 10s);
    ASSERT_EQ(got.size(), 1U);
    std::this_thread::sleep_for(500ms);
    got = receiver_->wait("/data-change", 1, 1s);
    EXPECT_EQ(got.size(), 1U);
    parts = mp::parse_any(got[0].headers.find("content-type")->second, got[0].body);
    EXPECT_EQ(json::parse((*parts)[0].body)["operationType"], "DELETED");

    // Unsubscribe: client-id as form-exploded ClientId; the wrong owner is refused.
    EXPECT_EQ(send("DELETE", base() + "/subs-to-notify/sub1").status, 400);
    EXPECT_EQ(cause(send("DELETE", base() + "/subs-to-notify/sub1?nfSetId=set-x")),
              "SUBSCRIPTION_EXISTS");
    r = send(
        "DELETE",
        base() +
            "/subs-to-notify/sub1?nfId=11111111-1111-4111-8111-111111111111&get-previous=true");
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).get<std::vector<sbi_gen::NotificationSubscription>>().size(), 1U);
    EXPECT_EQ(cause(send("GET", base() + "/subs-to-notify/sub1")), "SUBSCRIPTION_NOT_FOUND");
}

// 5.2.2.6.2 Record Expiry Notify and 5.2.2.6.4 Subscription Expiry Notification.
TEST_F(Udsf, RecordAndSubscriptionExpiry) {
    receiver_->clear();
    const auto soon = [](int s) {
        return sbi_core::format_rfc3339(std::chrono::system_clock::now() + std::chrono::seconds(s));
    };
    ASSERT_EQ(put_record("e1",
                         json{{"ttl", soon(1)},
                              {"callbackReference", std::string(kReceiver) + "/record-expired"},
                              {"tags", {{"t", {"e"}}}}},
                         {block("b", "text/plain", "last words")})
                  .status,
              201);
    auto got = receiver_->wait("/record-expired", 1, 10s);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_NE(got[0].headers.find("content-location")->second.find("/records/e1"),
              std::string::npos);
    auto parts = mp::parse_any(got[0].headers.find("content-type")->second, got[0].body);
    ASSERT_TRUE(parts);
    ASSERT_EQ(parts->size(), 2U);
    EXPECT_EQ((*parts)[1].body, "last words");
    EXPECT_EQ(send("GET", base() + "/records/e1").status, 404);
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(receiver_->wait("/record-expired", 2, 1s).size(), 1U); // exactly once, 2 replicas

    const json sub{{"clientId", {{"nfSetId", "set-e"}}},
                   {"callbackReference", std::string(kReceiver) + "/data-change"},
                   {"expiryCallbackReference", std::string(kReceiver) + "/sub-expiry"},
                   {"expiry", soon(2)},
                   {"expiryNotification", 0}};
    ASSERT_EQ(send("PUT", base() + "/subs-to-notify/exp1", sub.dump(), "application/json").status,
              201);
    got = receiver_->wait("/sub-expiry", 1, 10s);
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].headers.find("3gpp-sbi-callback")->second,
              "Nudsf_DataRepository_subscriptionExpiryNotification");
    const auto info =
        json::parse(got[0].body).get<sbi_gen::NotificationInfo_Nudsf_DataRepository>();
    ASSERT_EQ(info.expiredSubscriptions.size(), 1U);
    EXPECT_EQ(info.expiredSubscriptions[0].clientId.nfSetId, "set-e");
    EXPECT_EQ(send("GET", base() + "/subs-to-notify/exp1").status, 404);
}

// Meta Schema resource (5.2.2.2.9, 5.2.2.3.4, 5.2.2.4.7, 5.2.2.5.4).
TEST_F(Udsf, MetaSchemaLifecycle) {
    const json schema{
        {"schemaId", "sch1"},
        {"metaTags",
         json::array({{{"tagName", "supi"}, {"keyType", "SEARCH_KEY"}, {"sort", true}}})}};
    auto r = send("PUT", base() + "/meta-schemas/sch1", schema.dump(), "application/json");
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(json::parse(r.body).get<sbi_gen::MetaSchema>().schemaId, "sch1");
    r = send("GET", base() + "/meta-schemas/sch1");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(header_of(r, "content-type"), "application/json");
    EXPECT_EQ(json::parse(r.body).get<sbi_gen::MetaSchema>().metaTags.size(), 1U);
    json bad = schema;
    bad["metaTags"][0].erase("sort");
    EXPECT_EQ(send("PUT", base() + "/meta-schemas/sch1", bad.dump(), "application/json").status,
              400);
    EXPECT_EQ(send("PUT", base() + "/meta-schemas/other", schema.dump(), "application/json").status,
              400);
    EXPECT_EQ(send("PUT", base() + "/meta-schemas/sch1", schema.dump(), "application/json").status,
              204);
    // Referenced by a timer -> SCHEMA_IN_USE.
    const json timer{{"expires", sbi_core::format_rfc3339(std::chrono::system_clock::now() + 1h)},
                     {"schemaId", "sch1"}};
    ASSERT_EQ(send("PUT", tbase() + "/timers/ts1", timer.dump(), "application/json").status, 201);
    EXPECT_EQ(cause(send("DELETE", base() + "/meta-schemas/sch1")), "SCHEMA_IN_USE");
    ASSERT_EQ(send("DELETE", tbase() + "/timers/ts1").status, 204);
    r = send("DELETE", base() + "/meta-schemas/sch1?get-previous=true");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(cause(send("GET", base() + "/meta-schemas/sch1")), "SCHEMA_NOT_FOUND");
}

// Nudsf_Timer: 5.3.2.2.2 Start, 5.3.2.3.2 Update, 5.3.2.4.2/3 Stop, 5.3.2.5.2/3 Search,
// 5.3.2.6.2 Notify (with PeriodicTimer and deleteAfter).
TEST_F(Udsf, TimerService) {
    receiver_->clear();
    const auto at = [](std::chrono::milliseconds d) {
        return sbi_core::format_rfc3339(std::chrono::system_clock::now() + d);
    };
    const json t1{{"expires", at(1h)},
                  {"metaTags", {{"supi", {"imsi-9"}}}},
                  {"callbackReference", std::string(kReceiver) + "/timer"}};
    auto r = send("PUT", tbase() + "/timers/t1", t1.dump(), "application/json");
    ASSERT_EQ(r.status, 201) << r.body;
    EXPECT_EQ(send("PUT", tbase() + "/timers/t1", t1.dump(), "application/json").status, 204);
    r = send("GET", tbase() + "/timers/t1");
    ASSERT_EQ(r.status, 200);
    const auto got = json::parse(r.body).get<sbi_gen::Timer>();
    EXPECT_FALSE(got.timerId.has_value());
    json past = t1;
    past["expires"] = at(-1h);
    EXPECT_EQ(cause(send("PUT", tbase() + "/timers/t2", past.dump(), "application/json")),
              "EXPIRES_VALUE_NOT_ALLOWED");
    json with_id = t1;
    with_id["timerId"] = "t1";
    EXPECT_EQ(send("PUT", tbase() + "/timers/t2", with_id.dump(), "application/json").status, 400);

    // Tagged search / expired search.
    r = send("GET",
             tbase() + "/timers?filter=" +
                 pct(json{{"op", "EQ"}, {"tag", "supi"}, {"value", "imsi-9"}}.dump()));
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body).get<sbi_gen::TimerIdList>().timerIds,
              std::vector<std::string>{"t1"});
    EXPECT_EQ(send("GET", tbase() + "/timers?expired-filter=null").status, 204);
    EXPECT_EQ(send("GET", tbase() + "/timers").status, 400);

    // PATCH: move it to expire in 1 s, repeating twice every second.
    const json patch = json::array({{{"op", "replace"}, {"path", "/expires"}, {"value", at(1s)}},
                                    {{"op", "add"}, {"path", "/repetitionCount"}, {"value", 2}},
                                    {{"op", "add"}, {"path", "/periodicRepetition"}, {"value", 1}},
                                    {{"op", "add"}, {"path", "/deleteAfter"}, {"value", 30}}});
    r = send("PATCH", tbase() + "/timers/t1", patch.dump(), "application/json-patch+json");
    ASSERT_EQ(r.status, 204) << r.body;
    auto fired = receiver_->wait("/timer", 3, 15s);
    ASSERT_EQ(fired.size(), 3U);
    for (const auto& f : fired) {
        EXPECT_EQ(f.headers.find("3gpp-sbi-callback")->second, "Nudsf_Timer_timerExpiry");
        const auto t = json::parse(f.body).get<sbi_gen::Timer>();
        EXPECT_EQ(t.timerId, "t1");
        EXPECT_FALSE(t.callbackReference.has_value());
    }
    std::this_thread::sleep_for(1500ms);
    EXPECT_EQ(receiver_->wait("/timer", 4, 1s).size(), 3U); // repetitionCount honoured, once each
    // deleteAfter keeps it, now expired.
    r = send("GET", tbase() + "/timers?expired-filter=null");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(json::parse(r.body)["timerIds"], json::array({"t1"}));

    // Stop: single and by filter.
    ASSERT_EQ(send("PUT",
                   tbase() + "/timers/t3",
                   json{{"expires", at(1h)}, {"metaTags", {{"g", {"x"}}}}}.dump(),
                   "application/json")
                  .status,
              201);
    ASSERT_EQ(send("PUT",
                   tbase() + "/timers/t4",
                   json{{"expires", at(1h)}, {"metaTags", {{"g", {"x"}}}}}.dump(),
                   "application/json")
                  .status,
              201);
    r = send("DELETE",
             tbase() +
                 "/timers?filter=" + pct(json{{"op", "EQ"}, {"tag", "g"}, {"value", "x"}}.dump()));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body).get<sbi_gen::TimerDeleteResponse>().timerIds,
              (std::vector<std::string>{"t3", "t4"}));
    ASSERT_EQ(send("DELETE", tbase() + "/timers/t1").status, 204);
    EXPECT_EQ(cause(send("DELETE", tbase() + "/timers/t1")), "TIMER_NOT_FOUND");
    EXPECT_EQ(
        cause(send("PATCH", tbase() + "/timers/t1", patch.dump(), "application/json-patch+json")),
        "TIMER_NOT_FOUND");
    EXPECT_EQ(send("DELETE", tbase() + "/timers?expired-filter=null").status, 204);
}

// TS 29.500 6.7.3: an invalid token is 401 + WWW-Authenticate; a token without the service
// scope is 403 insufficient_scope; the right scope passes.
TEST_F(Udsf, OAuth2Enforcement) {
    auto r = send("GET", base() + "/records/none", "", "", {{"authorization", "Bearer not.a.jwt"}});
    EXPECT_EQ(r.status, 401);
    EXPECT_NE(header_of(r, "www-authenticate").find("invalid_token"), std::string::npos);
    const auto wrong = token(client_, "nudsf-timer", "UDSF");
    ASSERT_FALSE(wrong.empty());
    r = send("GET", base() + "/records/none", "", "", {{"authorization", "Bearer " + wrong}});
    EXPECT_EQ(r.status, 403);
    EXPECT_NE(header_of(r, "www-authenticate").find("insufficient_scope"), std::string::npos);
    const auto right = token(client_, "nudsf-dr", "UDSF");
    r = send("GET", base() + "/records/none", "", "", {{"authorization", "Bearer " + right}});
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(cause(r), "RECORD_NOT_FOUND");
}

// The UDSF registers with the NRF and is discoverable with its UdsfInfo and both services.
TEST_F(Udsf, RegisteredAndDiscoverable) {
    const auto tok = token(client_, "nnrf-disc", "NRF");
    ASSERT_FALSE(tok.empty());
    json instances;
    for (int i = 0; i < 50 && instances.empty(); ++i) {
        auto r = send("GET",
                      std::string(kNrf) +
                          "/nnrf-disc/v1/nf-instances?target-nf-type=UDSF&requester-nf-type=AMF",
                      "",
                      "",
                      {{"authorization", "Bearer " + tok}});
        if (r.status == 200) {
            instances = json::parse(r.body).value("nfInstances", json::array());
        }
        if (instances.empty()) {
            std::this_thread::sleep_for(200ms);
        }
    }
    ASSERT_GE(instances.size(), 1U);
    const auto& p = instances[0];
    EXPECT_EQ(p["nfType"], "UDSF");
    EXPECT_TRUE(p["udsfInfo"]["storageIdRanges"].contains(realm()));
    std::set<std::string> names;
    for (const auto& s : p["nfServices"]) {
        names.insert(s["serviceName"]);
    }
    EXPECT_EQ(names, (std::set<std::string>{"nudsf-dr", "nudsf-timer"}));
}

} // namespace
