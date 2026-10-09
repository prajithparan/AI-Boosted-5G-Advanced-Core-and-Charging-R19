// ADR-0478: the opt-in TLS 1.2 floor (SBI_TLS_MIN_VERSION / config key tls_min_version).
// Default must stay TLS 1.3 only; "1.2" must let a TLS 1.2-only peer in, with AEAD-ECDHE suites.
#include "sbi_core/http2_client.hpp"
#include "sbi_core/http2_server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <cstdlib>
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

// Runs a server on an ephemeral port while in scope.
struct RunningServer {
    boost::asio::io_context ioc;
    sbi_core::http2::Server server;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
    std::thread thread;
    RunningServer()
        : server(ioc, "127.0.0.1", 0, test_tls()), guard(boost::asio::make_work_guard(ioc)) {
        server.add_route("GET", "/ping", [](const sbi_core::http2::Request&) {
            return sbi_core::http2::Response::json(200, R"({"ok":true})");
        });
        server.start();
        thread = std::thread([this] { ioc.run(); });
    }
    ~RunningServer() {
        guard.reset();
        ioc.stop();
        thread.join();
    }
    unsigned short port() const { return server.local_port(); }
};

// Handshake from a client that can ONLY speak TLS 1.2 (presenting the lab client cert for mTLS).
// Returns the cipher name on success, "" on handshake failure.
std::string tls12_only_handshake(unsigned short port) {
    namespace ssl = boost::asio::ssl;
    boost::asio::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    const auto t = test_tls();
    ctx.use_certificate_chain_file(t.cert_path);
    ctx.use_private_key_file(t.key_path, ssl::context::pem);
    ctx.load_verify_file(t.ca_path);
    ctx.set_verify_mode(ssl::verify_peer);
    ssl::stream<boost::asio::ip::tcp::socket> s(ioc, ctx);
    boost::system::error_code ec;
    s.lowest_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
    if (ec) {
        return "";
    }
    s.handshake(ssl::stream_base::client, ec);
    if (ec) {
        return "";
    }
    return SSL_get_cipher_name(s.native_handle());
}

} // namespace

TEST(SbiTlsMinVersion, DefaultRejectsATls12OnlyPeer) {
    ::unsetenv("SBI_TLS_MIN_VERSION");
    RunningServer srv;
    EXPECT_EQ(tls12_only_handshake(srv.port()), "");
}

TEST(SbiTlsMinVersion, OptInAcceptsTls12WithAeadEcdheOnly) {
    ::setenv("SBI_TLS_MIN_VERSION", "1.2", 1);
    {
        RunningServer srv;
        const auto cipher = tls12_only_handshake(srv.port());
        EXPECT_FALSE(cipher.empty());
        EXPECT_NE(cipher.find("ECDHE"), std::string::npos) << cipher;
        EXPECT_TRUE(cipher.find("GCM") != std::string::npos ||
                    cipher.find("CHACHA20") != std::string::npos)
            << cipher;
        // The project's own client (floor 1.2, negotiates 1.3 with a 1.3-capable server) still
        // works.
        sbi_core::http2::Client client(test_tls());
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = "https://127.0.0.1:" + std::to_string(srv.port()) + "/ping";
        auto resp = client.send(req);
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(resp->status, 200);
    }
    ::unsetenv("SBI_TLS_MIN_VERSION");
}

TEST(SbiTlsMinVersion, UnknownValueFailsClosed) {
    ::setenv("SBI_TLS_MIN_VERSION", "1.1", 1);
    EXPECT_THROW(sbi_core::http2::min_tls_version(), std::runtime_error);
    ::unsetenv("SBI_TLS_MIN_VERSION");
}
