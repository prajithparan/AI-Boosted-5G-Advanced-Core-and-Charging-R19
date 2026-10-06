#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "li_core/x1.hpp"
#include "li_core/x2x3_pdu.hpp"

// The generic runtime of a POI (ADR-0463), extracted from the AMF's IRI-POI (ADR-0377/0461) so the
// SMF and every later POI share one implementation instead of copying 560 lines each.
//
// What it owns: the LI_X1 listener (TS 103 221-1 7.2.2.2, POST /X1/NE over TLS 1.3 + mTLS), the
// warrant store the ADMF provisions into, the clause-6.6.2 keepalive monitor, the per-XID sequence
// numbers, the LI_X2 delivery (one TS 103 221-2 PDU per xIRI), and the worker that runs a POI's
// "targets added" callback off the X1 request path.
//
// What it does not know: which identities a POI matches, which xIRI records it builds, when it
// builds them. Those stay in the NF (nfs/amf/src/li_poi.cpp, nfs/smf/src/li_poi.cpp).
namespace li_poi {

struct Config {
    std::string x1_bind_address; // LI_X1 listener bind address
    std::uint16_t x1_port = 0;
    std::string ne_identifier;         // echoed on X1 responses
    std::string network_function_id;   // X2 NFID attribute (TS 33.128 5.3.1)
    std::string interception_point_id; // X2 IPID attribute
    std::string mdf2_host;             // LI_X2 destination (the MDF2)
    std::uint16_t mdf2_port = 0;
    std::string mdf2_sni;  // server name to require in the MDF2's certificate
    std::string cert_path; // PEM: mTLS material (shared lab PKI)
    std::string key_path;
    std::string ca_path;
    int x1_keepalive_p1_seconds = 60; // TS 103 221-1 6.6.2 timers
    int x1_keepalive_p2_seconds = 180;
    int x1_keepalive_p3_seconds = 300;
    bool x1_allow_deactivate_all = true;
};

// One warrant that targets an identity: its XID, the target identifier that matched (for the X2
// Matched Target Identifier attribute) and the per-task identifier-association gating
// (TS 33.128 table 6.2.2.1.1-1; only the AMF reads it).
struct Match {
    std::string xid;
    li_core::x1::TargetIdentifier target;
    std::optional<li_core::x1::IdentifierAssociationEventsGenerated> identifier_association;
};

struct TaskInfo {
    std::vector<li_core::x1::TargetIdentifier> targets;
    std::optional<li_core::x1::IdentifierAssociationEventsGenerated> identifier_association;
    // What the ADMF asked this task to deliver (IRI, CC or both) and to which Destination IDs. A
    // POI that is also a trigger function (the SMF's CC-TF) needs them to trigger the UPF.
    li_core::x1::DeliveryType delivery = li_core::x1::DeliveryType::X2AndX3;
    std::vector<std::string> dids;
};

struct Hooks {
    std::string log_name = "li-poi"; // prefix of every log line ("amf-li-poi")
    // The target identifier kinds this POI can match. ActivateTask / ModifyTask naming any other
    // kind is refused with X1 error 3010 (UnsupportedTargetIdentifier). Empty = no validation.
    std::vector<li_core::x1::TargetIdentifierKind> supported_kinds;
    // Applied to a target value and to a queried identity before comparing (e.g. strip "imsi-").
    std::function<std::string(const std::string&)> normalise;
    // Called on the runtime's worker thread -- never on the X1 request path -- with the target
    // identifiers a task was activated with or a ModifyTask newly added.
    std::function<void(const std::string& xid,
                       const std::vector<li_core::x1::TargetIdentifier>& added)>
        on_targets_added;
    // Called on the same worker thread after a task is deactivated (DeactivateTask, or
    // DeactivateAllTasks once per task), so a trigger function can withdraw what it triggered.
    std::function<void(const std::string& xid)> on_task_removed;
};

class PoiRuntime {
public:
    PoiRuntime(Config config, Hooks hooks);
    ~PoiRuntime();
    PoiRuntime(const PoiRuntime&) = delete;
    PoiRuntime& operator=(const PoiRuntime&) = delete;

    // Start the LI_X1 listener, the keepalive monitor and the worker. The X2 client connects
    // lazily.
    void start();
    void stop();

    // EVERY task that targets `identity` through a target of one of `kinds` (one Match per task:
    // two warrants on one subscriber are each reported under their own XID).
    [[nodiscard]] std::vector<Match>
    matches(const std::string& identity,
            const std::vector<li_core::x1::TargetIdentifierKind>& kinds) const;
    [[nodiscard]] std::optional<TaskInfo> task(const std::string& xid) const;

    // Wrap one already-encoded xIRI payload (BER XIRIPayload) into a TS 103 221-2 X2 PDU for
    // `match` and send it. Best-effort: a delivery failure is logged and counted, never propagated
    // into the procedure that triggered it. `record` names the xIRI record for the log.
    void emit(const Match& match,
              std::span<const std::uint8_t> payload,
              li_core::PayloadDirection direction,
              const std::string& record);

    // The CC counterpart of emit(): one user-plane packet as a TS 103 221-2 X3 PDU (clause 5.4: the
    // `format` is 5 IPv4, 6 IPv6, 7 Ethernet or 12 GTP-U) to the configured delivery endpoint --
    // for a CC-POI that is the MDF3. Best-effort, like emit().
    void emit_cc(const std::string& xid,
                 li_core::PayloadFormat format,
                 li_core::PayloadDirection direction,
                 std::span<const std::uint8_t> packet);

    [[nodiscard]] const Config& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// TS 103 221-2 clause 5.2.7 carries the XID as a 128-bit integer, the X1 XSD writes it as a UUID
// string: parse one into the other.
std::optional<std::array<std::uint8_t, 16>> uuid_to_bytes(const std::string& uuid);

} // namespace li_poi
