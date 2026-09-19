#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <sw/redis++/redis++.h>
#include <vector>

#include "li_core/hi2.hpp"
#include "li_core/x1.hpp"

// The MDF2's X1-provisioned state, in Valkey (ADR-0377; the ADR-0359 rule: no in-process state,
// so a second MDF2 replica sees the same warrants). Three kinds of key:
//
//   limdf:task:<xid>    one ETSI TS 103 221-1 TaskDetails as the ADMF provisioned it: the LIID
//                       the MDF2 stamps into every PS-PDU for that XID, the target identifiers
//                       (clause 5.5.5 "lEAProvided"), and the Destination IDs its records go to
//   limdf:tasks         the set of live XIDs, so DeactivateAllTasks and a restart can enumerate
//   limdf:dest:<did>    one DestinationDetails: where the LEMF is for that Destination ID
//   limdf:dests         the set of live DIDs
//   limdf:seq:<liid>    INCR counter behind PSHeader.sequenceNumber, per LIID rather than per
//                       connection, because TS 102 232-1 clause 5.2.5 numbers the records of one
//                       interception, not the socket that happens to carry them
//
// An X2 PDU carries the XID the POI was provisioned with (TS 103 221-2 clause 5.2.7), which is
// how a received record finds its warrant here.

namespace li_mdf {

// One LIID of a task, with where its records go. TS 103 221-1 clause 5.1.2: an XID may map to one
// LIID or to several, and "the ADMF shall provide the XID to LIID(s) mapping to the MDF" -- it
// does so in the Annex C.2.2 MediationDetails structures, one per LIID. Each carries its own
// HI2/HI3 choice, and its own Destination list when it deviates from the task's.
struct MediationRoute {
    std::string liid;
    li_core::x1::MediationDeliveryType delivery = li_core::x1::MediationDeliveryType::Hi2AndHi3;
    std::vector<std::string> dids; // already resolved: the route's own, else the task's
};

// The subset of a TaskDetails an MDF2 acts on. The X1 codec parses more than this; what is stored
// is what mediation and delivery need, plus the raw target identifiers for provenance.
struct Task {
    std::string xid;
    std::vector<MediationRoute> routes; // one per LIID; empty is refused at activation
    std::vector<li_core::x1::TargetIdentifier> targets;
    li_core::x1::DeliveryType delivery = li_core::x1::DeliveryType::X2AndX3;
};
void to_json(nlohmann::json& j, const MediationRoute& v);
void from_json(const nlohmann::json& j, MediationRoute& v);
void to_json(nlohmann::json& j, const Task& v);
void from_json(const nlohmann::json& j, Task& v);

struct Destination {
    std::string did;
    std::string host;
    std::uint16_t port = 0;
    li_core::x1::DeliveryType delivery = li_core::x1::DeliveryType::X2AndX3;
};
void to_json(nlohmann::json& j, const Destination& v);
void from_json(const nlohmann::json& j, Destination& v);

class TaskStore {
public:
    explicit TaskStore(sw::redis::Redis& redis) : redis_(redis) {}

    // Each returns the X1 error code to report, or nullopt on success -- the shape
    // li_core::x1::TaskStoreCallbacks wants.
    std::optional<li_core::x1::ErrorCode> activate(const li_core::x1::TaskDetails& details);
    std::optional<li_core::x1::ErrorCode> modify(const li_core::x1::TaskDetails& details);
    std::optional<li_core::x1::ErrorCode> deactivate(const std::string& xid);
    std::optional<li_core::x1::ErrorCode> deactivate_all();
    std::optional<li_core::x1::ErrorCode>
    create_destination(const li_core::x1::DestinationDetails& details);
    std::optional<li_core::x1::ErrorCode> remove_destination(const std::string& did);
    std::optional<li_core::x1::ErrorCode> remove_all_destinations();

    [[nodiscard]] std::optional<Task> task(const std::string& xid);
    [[nodiscard]] std::optional<Destination> destination(const std::string& did);
    [[nodiscard]] std::vector<Destination> destinations();
    // PSHeader.sequenceNumber for the next record of this warrant.
    [[nodiscard]] std::uint32_t next_sequence(const std::string& liid);

private:
    sw::redis::Redis& redis_;
};

// The X1 target identifiers of a task, tagged with the clause-5.5.5 provenance the MDF2 can
// honestly claim for them: they came from the provisioning message, so "lEAProvided".
std::vector<li_core::hi2::IriTargetIdentifier> lea_provided(const Task& task);

} // namespace li_mdf
