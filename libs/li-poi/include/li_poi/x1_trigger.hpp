#pragma once

#include "sbi_core/http2_client.hpp"

#include <string>
#include <tl/expected.hpp>
#include <vector>

#include "li_core/x1.hpp"

// The triggering side of LI_T2/LI_T3 (TS 33.128 clause 5.2.5, 6.2.3.3): the IRI-TF / CC-TF in the
// SMF drives a POI in the UPF with the same X1 protocol the ADMF uses (ETSI TS 103 221-1 over
// HTTP/2 + mTLS). One instance per triggered POI.
namespace li_poi {

struct TriggerConfig {
    std::string tf_identifier; // goes in the X1 admfIdentifier field: who is sending
    std::string ne_identifier; // the triggered POI's neIdentifier
    std::string x1_url;        // https://<upf>:<port>/X1/NE
    std::string x1_version = "v1.23.1";
};

class X1Trigger {
public:
    X1Trigger(TriggerConfig config, sbi_core::http2::TlsConfig tls);

    // ActivateTask for `xid` on `targets`, X3-only delivery to `dids` (TS 33.128 6.2.3.3.1).
    tl::expected<void, std::string>
    activate(const std::string& xid,
             const std::vector<li_core::x1::TargetIdentifier>& targets,
             const std::vector<std::string>& dids);
    // ModifyTask: the task's full target list replaced (TS 33.128 6.2.3.3.1, table 6.2.3.3.1-3).
    tl::expected<void, std::string>
    modify(const std::string& xid,
           const std::vector<li_core::x1::TargetIdentifier>& targets,
           const std::vector<std::string>& dids);
    tl::expected<void, std::string> deactivate(const std::string& xid);

    [[nodiscard]] const TriggerConfig& config() const { return config_; }

private:
    tl::expected<void, std::string> send(li_core::x1::RequestBody body);
    tl::expected<void, std::string>
    send_task(bool activate,
              const std::string& xid,
              const std::vector<li_core::x1::TargetIdentifier>& targets,
              const std::vector<std::string>& dids);
    TriggerConfig config_;
    sbi_core::http2::Client client_;
};

} // namespace li_poi
