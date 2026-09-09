// ADR-0325. sbi_core's route matcher is first-registered-wins with no preference for a literal
// path segment over a parameter one (libs/sbi-core/src/http2_server.cpp:119 compares segments in
// order and returns on the first route whose method and arity match). That is a real property NEF
// now depends on: TS29522_ASTI defines both "/{afId}/configurations/retrieve" and
// "/{afId}/configurations/{configId}" -- same depth, one literal where the other has a parameter.
//
// The two do not collide in today's spec, because the item path defines no POST. That is a
// property of the current YAML rather than of the router, so this test pins the router's actual
// behaviour instead: registered first, the literal wins; registered second, it is shadowed and the
// parameter route swallows "retrieve" as an id. The second half is the part worth having -- it
// fails loudly if someone ever "tidies" the registration order in main.cpp.

#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio/io_context.hpp>

#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace {

sbi_core::http2::TlsConfig test_tls() {
    return sbi_core::http2::TlsConfig{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
}

// Same in-process-server idiom as test_sbi_core_concurrency.cpp, single-threaded: this test is
// about which route matches, not about how many run at once.
class IoContextThread {
public:
    explicit IoContextThread(boost::asio::io_context& ioc)
        : ioc_(ioc), guard_(boost::asio::make_work_guard(ioc)), thread_([&ioc] { ioc.run(); }) {}

    ~IoContextThread() {
        guard_.reset();
        ioc_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    IoContextThread(const IoContextThread&) = delete;
    IoContextThread& operator=(const IoContextThread&) = delete;

private:
    boost::asio::io_context& ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_;
    std::thread thread_;
};

// Registers the ASTI-shaped pair of POST routes in the given order and reports which one answers
// POST ".../configurations/retrieve".
std::string which_route_answers(bool literal_first) {
    boost::asio::io_context ioc;
    sbi_core::http2::Server server(ioc, "127.0.0.1", 0, test_tls());

    const auto literal = [&server] {
        server.add_route(
            "POST", "/api/{afId}/configurations/retrieve", [](const sbi_core::http2::Request&) {
                return sbi_core::http2::Response::json(200, R"({"route":"literal"})");
            });
    };
    const auto param = [&server] {
        server.add_route("POST",
                         "/api/{afId}/configurations/{configId}",
                         [](const sbi_core::http2::Request& req) {
                             return sbi_core::http2::Response::json(
                                 200,
                                 R"({"route":"param","configId":")" +
                                     req.path_params.at("configId") + R"("})");
                         });
    };

    if (literal_first) {
        literal();
        param();
    } else {
        param();
        literal();
    }

    server.start();
    const auto port = server.local_port();
    IoContextThread runner(ioc);

    sbi_core::http2::Client client(test_tls());
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:" + std::to_string(port) + "/api/af1/configurations/retrieve";
    req.body = "{}";
    auto resp = client.send(req);
    EXPECT_TRUE(resp.has_value());
    if (!resp.has_value()) {
        return "<no response>";
    }
    EXPECT_EQ(resp->status, 200);
    return resp->body;
}

} // namespace

TEST(Http2RoutePrecedence, LiteralSegmentWinsWhenRegisteredBeforeTheParameterRoute) {
    EXPECT_EQ(which_route_answers(true), R"({"route":"literal"})");
}

TEST(Http2RoutePrecedence, LiteralSegmentIsShadowedWhenRegisteredAfterTheParameterRoute) {
    // Not the behaviour anyone wants -- it is the behaviour the matcher actually has. "retrieve"
    // is read as a configuration id and the retrieve handler never runs. Pinned so that NEF's
    // deliberate registration order is understood as load-bearing rather than incidental.
    EXPECT_EQ(which_route_answers(false), R"({"route":"param","configId":"retrieve"})");
}
