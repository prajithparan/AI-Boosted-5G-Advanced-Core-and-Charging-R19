#include "sbi_core/http2_client.hpp"

#include <curl/curl.h>

#include <mutex>

namespace sbi_core::http2 {

namespace {

// Upper bound on IDLE handles retained between calls. Concurrency above this still works (checkout
// creates on demand); it just stops the pool retaining more than this many afterwards, so a single
// burst can't leave hundreds of connections parked open for the process's lifetime.
constexpr std::size_t kMaxIdleHandles = 64;

void ensure_curl_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t write_body_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

std::size_t write_header_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::multimap<std::string, std::string>*>(userdata);
    const std::string line(ptr, size * nmemb);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
                s.pop_back();
            }
            std::size_t start = s.find_first_not_of(' ');
            s = (start == std::string::npos) ? "" : s.substr(start);
        };
        trim(value);
        headers->emplace(std::move(name), std::move(value));
    }
    return size * nmemb;
}

} // namespace

Client::Client(TlsConfig tls) : tls_(std::move(tls)) {
    ensure_curl_global_init();
}

Client::~Client() {
    // No lock: per the lifetime contract in the header, a Client must outlive every in-flight
    // send() on it, so by here no other thread can be touching idle_.
    for (void* handle : idle_) {
        curl_easy_cleanup(static_cast<CURL*>(handle));
    }
}

Client::HandleLease::~HandleLease() {
    if (handle_ != nullptr) {
        owner_.checkin(handle_);
    }
}

void* Client::checkout() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idle_.empty()) {
            void* handle = idle_.back();
            idle_.pop_back();
            return handle;
        }
    }
    // Pool empty -- create outside the lock; curl_easy_init can fail (allocation), so the caller
    // still has to handle nullptr rather than assume a handle exists.
    return curl_easy_init();
}

void Client::checkin(void* handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idle_.size() < kMaxIdleHandles) {
            idle_.push_back(handle);
            return;
        }
    }
    // Pool at capacity: destroy rather than grow without bound. Only reachable if concurrency
    // genuinely exceeded kMaxIdleHandles, in which case losing this handle's keepalive state is
    // the cheaper outcome.
    curl_easy_cleanup(static_cast<CURL*>(handle));
}

tl::expected<ClientResponse, std::string> Client::send(const ClientRequest& request) {
    HandleLease lease(*this, checkout());
    if (lease.get() == nullptr) {
        return tl::unexpected("curl handle not initialized");
    }
    auto* curl = static_cast<CURL*>(lease.get());
    // Deliberate: curl_easy_reset clears the per-request options but PRESERVES this handle's live
    // connection, TLS session cache and DNS cache -- that preservation is what makes pooled reuse
    // real HTTP/2 keepalive rather than a fresh connection per call.
    curl_easy_reset(curl);

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    // CURL_HTTP_VERSION_2TLS: negotiate HTTP/2 via ALPN over TLS, as opposed to
    // CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE (cleartext h2c, no longer used -- see ADR-0011).
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());

    // TLS 1.3 + mTLS, non-negotiable (CLAUDE.md). No option here disables verification -- if the
    // cert/key/CA paths are wrong this fails closed (curl_easy_perform returns an error), not open.
    if (min_tls_version() == MinTlsVersion::v1_2) {
        // ADR-0478 opt-in: floor 1.2 (curl treats this as "1.2 or newer"), AEAD-ECDHE suites only.
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, kTls12CipherList);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3);
    }
    curl_easy_setopt(curl, CURLOPT_SSLCERT, tls_.cert_path.c_str());
    curl_easy_setopt(curl, CURLOPT_SSLKEY, tls_.key_path.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, tls_.ca_path.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (!request.body.empty() || request.method == "PUT" || request.method == "POST" ||
        request.method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }

    curl_slist* header_list = nullptr;
    for (const auto& [name, value] : request.headers) {
        const std::string line = name + ": " + value;
        header_list = curl_slist_append(header_list, line.c_str());
    }
    if (header_list != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    ClientResponse response;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    const CURLcode res = curl_easy_perform(curl);

    if (header_list != nullptr) {
        curl_slist_free_all(header_list);
    }

    if (res != CURLE_OK) {
        return tl::unexpected(std::string(curl_easy_strerror(res)));
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = status;

    return response;
}

} // namespace sbi_core::http2
