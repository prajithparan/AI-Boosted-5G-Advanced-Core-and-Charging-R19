#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "li_core/hi1.hpp"
#include "lifecycle.hpp"

// The ADMF's LI_HI1 receiver (ADR-0462): transport-independent request handling for ETSI TS 103
// 120. main.cpp feeds it the body of an HTTPS POST and the mTLS peer's identity; it returns the
// response body. Step 3 of the li-admf build: message-level checks (clause 9.2.2, 6.2.3, 6.4.4),
// the LEA binding, and GETCSPCONFIG. The six LI lifecycle workflows (Annex H.5) are step 4 -- until
// then every other action is answered with error 3001 (feature not supported), never silently
// accepted.
namespace li_admf {

// An LEA the CSP has onboarded: the mTLS client certificate CN allowed to speak for an HI1
// EndpointID. The national profile decides how an LEA is authenticated (TS 103 120 9.3.4); this
// project binds the transport identity to the SenderIdentifier it may use.
struct LeaBinding {
    std::string peer_cert_cn;
    li_core::hi1::EndpointId endpoint;
};

struct Hi1Config {
    li_core::hi1::EndpointId self; // this CSP's EndpointID (ReceiverIdentifier of requests)
    std::string national_profile_owner;
    std::string national_profile_version;
    std::vector<std::string> supported_etsi_versions; // ETSIVersion values accepted ("V1.23.1")
    std::string public_base_url;                      // advertised in GETCSPCONFIG workflow URLs
    std::vector<LeaBinding> leas;
};

struct Hi1Reply {
    int http_status = 200; // HTTP 200 for every HI1 outcome (9.3.3); 403 only for an unknown peer
    std::string content_type = "text/xml";
    std::string body;
    // What the audit trail records (nothing here is sent to the peer).
    std::string outcome = "rejected"; // ok | rejected | error
    std::string detail;
    std::string transaction_id;
    std::string sender;
    int actions = 0;
};

class Hi1Service {
public:
    // `lifecycle` may be null (message-level behaviour only: every action but GETCSPCONFIG is then
    // refused 3001). It is not owned and must outlive the service.
    explicit Hi1Service(Hi1Config config, Lifecycle* lifecycle = nullptr);

    // `peer_cert_cn` is the verified mTLS client certificate CN; `path` the request path, which
    // selects the workflow endpoint (TS 103 120 table H.0b).
    [[nodiscard]] Hi1Reply handle(std::string_view peer_cert_cn,
                                  std::string_view path,
                                  std::string_view content_type,
                                  const std::string& body) const;

    [[nodiscard]] const Hi1Config& config() const { return config_; }

private:
    Hi1Config config_;
    std::string config_last_changed_;
    Lifecycle* lifecycle_;
};

// A fresh RFC 9562 version-4 UUID (for a response whose request carried no readable transaction
// id).
std::string new_uuid();
// "YYYY-MM-DDTHH:MM:SS.ffffffZ" (QualifiedMicrosecondDateTime) / "YYYY-MM-DDTHH:MM:SSZ"
// (QualifiedDateTime).
std::string now_microsecond_timestamp();
std::string now_timestamp();

} // namespace li_admf
