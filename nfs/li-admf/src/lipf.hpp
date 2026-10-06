#pragma once

#include "sbi_core/http2_client.hpp"

#include <optional>
#include <string>
#include <tl/expected.hpp>
#include <vector>

#include "li_core/x1.hpp"

// The LIPF half of the ADMF (TS 33.127 clause 5.3.5.3, ADR-0462 step 4): the secure proxy that
// provisions POIs and MDFs over LI_X1 on the LICF's behalf. It owns no warrant policy -- it turns
// "provision this task" into the ordered X1 operations the network elements need, and undoes them
// if one fails, so a task is either fully provisioned or not at all.
namespace li_admf {

// A network element the ADMF provisions: where its LI_X1 listener is and what it is.
struct NetworkElement {
    std::string name;          // for logs and status ("amf-poi")
    std::string role;          // "poi" (an IRI-POI, or the SMF's IRI-POI + CC-TF), "mdf2" or "mdf3"
    std::string ne_identifier; // the neIdentifier its X1 server checks (TS 103 221-1 6.1)
    std::string x1_url;        // e.g. https://127.0.0.1:7807/X1/NE
    std::string
        peer_cert_cn; // the mTLS client certificate CN it uses when it calls the ADMF's /X1/ADMF
    // The X1 target-identifier elements this POI can match ("supiimsi", "peiImei", ...). The LIPF
    // gives a POI only the targets it supports and refuses a task no POI can carry. Empty = the AMF
    // set (supiimsi, supinai, imsi, nai), what every POI in this deployment could match before the
    // SMF.
    std::vector<std::string> target_elements;
    // A POI that also triggers CC interception (the SMF: its CC-TF drives the UPF's CC-POI over
    // LI_T3, ADR-0464). A task that needs CC is provisioned only through such a POI, and refused
    // rather than silently downgraded to IRI-only when none carries its targets.
    bool cc_capable = false;
};

// The X1 target-identifier element -> the HI1 target FormatName (TS 103 120 Annex C) it carries.
std::optional<std::string> hi1_format_for_element(const std::string& element);

// How a request reaches a network element. A seam, so the LIPF's logic is testable against the
// real X1 server codec in memory, and the production implementation is the mTLS HTTP/2 client.
class X1Transport {
public:
    virtual ~X1Transport() = default;
    // The response body, or an error string for a transport failure.
    virtual tl::expected<std::string, std::string> post(const NetworkElement& ne,
                                                        const std::string& body) = 0;
};

class HttpX1Transport : public X1Transport {
public:
    explicit HttpX1Transport(sbi_core::http2::TlsConfig tls) : client_(std::move(tls)) {}
    tl::expected<std::string, std::string> post(const NetworkElement& ne,
                                                const std::string& body) override;

private:
    sbi_core::http2::Client client_;
};

struct Destination {
    std::string address; // "ip:port" of the LEMF's handover endpoint
};

// Everything the LIPF needs to provision one LI task, already reduced from the HI1 LITask.
struct TaskSpec {
    std::string xid; // the LITask's ObjectIdentifier doubles as the X1 XID
    std::string liid;
    std::vector<li_core::x1::TargetIdentifier> targets;
    bool wants_iri = true;
    bool wants_cc = false;
    std::vector<Destination> destinations;
    std::optional<std::string> start_time; // QualifiedMicrosecondDateTime
    std::optional<std::string> end_time;
};

struct LipfConfig {
    std::string admf_identifier;
    std::string x1_version = "v1.23.1";
    // TS 33.128 table 6.2.2.1.1-1: whether the AMF POI generates Identifier(De)Association records.
    // A deployment policy, not something a warrant carries.
    std::optional<li_core::x1::IdentifierAssociationEventsGenerated> poi_identifier_association;
    // The master switch for CC tasks (config `cc_capable`). Off until real packet capture exists in
    // the UPF datapath (ADR-0464 stage 4): a CC warrant is then refused, never downgraded to IRI.
    bool cc_capable = false;
};

struct LipfResult {
    bool ok = false;
    std::string detail;          // what failed, for the LI task's InvalidReason / audit
    std::optional<int> x1_error; // the X1 error code, when an NE answered one
};

class Lipf {
public:
    Lipf(LipfConfig config, std::vector<NetworkElement> elements, X1Transport& transport)
        : config_(std::move(config)), elements_(std::move(elements)), transport_(transport) {}

    // Why this task cannot be provisioned in this deployment, before any NE is touched.
    [[nodiscard]] std::optional<std::string> infeasible(const TaskSpec& spec) const;

    // MDF2 destinations + MDF2 task, then every POI's task. All-or-nothing: on a failure everything
    // this call created is removed again. Re-provisioning an existing XID converges (modify).
    LipfResult provision(const TaskSpec& spec);
    // DeactivateTask on every NE, then RemoveDestination for this task's destinations. Idempotent:
    // an XID an NE no longer knows (X1 2020) counts as done.
    LipfResult deprovision(const TaskSpec& spec);
    // Remove destinations a task no longer uses (after a delivery change). Best effort.
    void retire_destinations(const std::string& xid, const std::vector<std::string>& addresses);
    // The task's destinations changed: create the new ones on the MDF2 and ModifyTask it.
    LipfResult change_delivery(const TaskSpec& spec);

    // TS 103 221-1 6.6.2: an X1 Keepalive to every NE. An NE that hears nothing from its ADMF
    // within TIME_P2 raises a fault and, by default, deactivates every task as a security measure
    // -- so an ADMF that does not send these takes its own interceptions down.
    struct KeepaliveResult {
        std::string ne;
        bool ok = false;
        std::string detail;
    };
    std::vector<KeepaliveResult> keepalive_all();

    // The destination id the LIPF uses for `address` under task `xid` (deterministic, so a retry
    // and a deprovision find the same one).
    [[nodiscard]] static std::string destination_id(const std::string& xid,
                                                    const std::string& address);

private:
    LipfResult
    send(const NetworkElement& ne, li_core::x1::MessageType type, li_core::x1::RequestBody body);
    [[nodiscard]] li_core::x1::TaskDetails mdf_task(const TaskSpec& spec,
                                                    const NetworkElement& ne) const;
    [[nodiscard]] static bool is_mdf(const NetworkElement& ne);
    [[nodiscard]] static bool mdf_serves(const NetworkElement& ne, const TaskSpec& spec);
    [[nodiscard]] static li_core::x1::DeliveryType mdf_delivery(const NetworkElement& ne);
    [[nodiscard]] li_core::x1::TaskDetails poi_task(const TaskSpec& spec,
                                                    const NetworkElement& ne) const;
    [[nodiscard]] static bool poi_accepts(const NetworkElement& ne, const std::string& element);

    LipfConfig config_;
    std::vector<NetworkElement> elements_;
    X1Transport& transport_;
};

} // namespace li_admf
