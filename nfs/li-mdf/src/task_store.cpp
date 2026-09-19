#include "task_store.hpp"

#include <charconv>
#include <iterator>

namespace li_mdf {

namespace {
constexpr const char* kTaskPrefix = "limdf:task:";
constexpr const char* kTaskIndex = "limdf:tasks";
constexpr const char* kDestPrefix = "limdf:dest:";
constexpr const char* kDestIndex = "limdf:dests";
constexpr const char* kSeqPrefix = "limdf:seq:";

const char* kind_name(li_core::x1::TargetIdentifierKind kind) {
    using K = li_core::x1::TargetIdentifierKind;
    switch (kind) {
        case K::SupiImsi:
            return "SupiImsi";
        case K::SupiNai:
            return "SupiNai";
        case K::Suci:
            return "Suci";
        case K::PeiImei:
            return "PeiImei";
        case K::PeiImeisv:
            return "PeiImeisv";
        case K::GpsiMsisdn:
            return "GpsiMsisdn";
        case K::GpsiNai:
            return "GpsiNai";
        case K::Imsi:
            return "Imsi";
        case K::Imei:
            return "Imei";
        case K::Msisdn:
            return "Msisdn";
        case K::Ipv4Address:
            return "Ipv4Address";
        case K::Ipv6Address:
            return "Ipv6Address";
        case K::Nai:
            return "Nai";
        case K::Other:
            break;
    }
    return "Other";
}

li_core::x1::TargetIdentifierKind kind_from(const std::string& name) {
    using K = li_core::x1::TargetIdentifierKind;
    for (auto kind : {K::SupiImsi,
                      K::SupiNai,
                      K::Suci,
                      K::PeiImei,
                      K::PeiImeisv,
                      K::GpsiMsisdn,
                      K::GpsiNai,
                      K::Imsi,
                      K::Imei,
                      K::Msisdn,
                      K::Ipv4Address,
                      K::Ipv6Address,
                      K::Nai}) {
        if (name == kind_name(kind)) {
            return kind;
        }
    }
    return K::Other;
}

const char* mediation_delivery_name(li_core::x1::MediationDeliveryType delivery) {
    switch (delivery) {
        case li_core::x1::MediationDeliveryType::Hi2Only:
            return "HI2Only";
        case li_core::x1::MediationDeliveryType::Hi3Only:
            return "HI3Only";
        case li_core::x1::MediationDeliveryType::Hi2AndHi3:
            break;
    }
    return "HI2andHI3";
}

li_core::x1::MediationDeliveryType mediation_delivery_from(const std::string& name) {
    if (name == "HI2Only") {
        return li_core::x1::MediationDeliveryType::Hi2Only;
    }
    if (name == "HI3Only") {
        return li_core::x1::MediationDeliveryType::Hi3Only;
    }
    return li_core::x1::MediationDeliveryType::Hi2AndHi3;
}

const char* delivery_name(li_core::x1::DeliveryType delivery) {
    switch (delivery) {
        case li_core::x1::DeliveryType::X2Only:
            return "X2Only";
        case li_core::x1::DeliveryType::X3Only:
            return "X3Only";
        case li_core::x1::DeliveryType::X2AndX3:
            break;
    }
    return "X2AndX3";
}

li_core::x1::DeliveryType delivery_from(const std::string& name) {
    if (name == "X2Only") {
        return li_core::x1::DeliveryType::X2Only;
    }
    if (name == "X3Only") {
        return li_core::x1::DeliveryType::X3Only;
    }
    return li_core::x1::DeliveryType::X2AndX3;
}

// TS 103 221-1 6.3.1.2: an IpAddressAndPort DeliveryAddress is "ip:port". Split it here so the
// delivery function gets a host and a port rather than re-parsing a string.
bool split_host_port(const std::string& value, std::string& host, std::uint16_t& port) {
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon + 1 >= value.size()) {
        return false;
    }
    host = value.substr(0, colon);
    // An IPv6 literal arrives bracketed; the brackets are not part of the address.
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    unsigned int parsed = 0;
    const std::string port_text = value.substr(colon + 1);
    const auto result =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
    if (result.ec != std::errc{} || parsed == 0 || parsed > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

// TS 103 221-1 Annex C.2.2: one MediationDetails per LIID, each with its own HI2/HI3 choice and,
// when it deviates, its own destination list -- "If included, the details shall be used instead of
// any delivery destinations specified in the ListOfDIDs field in the TaskDetails structure."
// A task with no MediationDetails carries no LIID, and an MDF cannot label a record without one;
// that is refused rather than given an invented identifier.
std::vector<MediationRoute> routes_from(const li_core::x1::TaskDetails& details) {
    std::vector<MediationRoute> routes;
    routes.reserve(details.mediation_details.size());
    for (const auto& mediation : details.mediation_details) {
        if (mediation.liid.empty()) {
            return {};
        }
        MediationRoute route;
        route.liid = mediation.liid;
        route.delivery = mediation.delivery;
        route.dids = mediation.dids.empty() ? details.dids : mediation.dids;
        routes.push_back(std::move(route));
    }
    return routes;
}
} // namespace

void to_json(nlohmann::json& j, const MediationRoute& v) {
    j = {{"liid", v.liid}, {"delivery", mediation_delivery_name(v.delivery)}, {"dids", v.dids}};
}

void from_json(const nlohmann::json& j, MediationRoute& v) {
    v.liid = j.at("liid");
    v.delivery = mediation_delivery_from(j.at("delivery"));
    v.dids = j.at("dids").get<std::vector<std::string>>();
}

void to_json(nlohmann::json& j, const Task& v) {
    nlohmann::json targets = nlohmann::json::array();
    for (const auto& target : v.targets) {
        targets.push_back({{"kind", kind_name(target.kind)},
                           {"element", target.element},
                           {"value", target.value}});
    }
    nlohmann::json routes = nlohmann::json::array();
    for (const auto& route : v.routes) {
        nlohmann::json one;
        to_json(one, route);
        routes.push_back(std::move(one));
    }
    j = {{"xid", v.xid},
         {"routes", routes},
         {"targets", targets},
         {"delivery", delivery_name(v.delivery)}};
}

void from_json(const nlohmann::json& j, Task& v) {
    v.xid = j.at("xid");
    v.routes.clear();
    for (const auto& route : j.at("routes")) {
        MediationRoute out;
        from_json(route, out);
        v.routes.push_back(std::move(out));
    }
    v.targets.clear();
    for (const auto& target : j.at("targets")) {
        li_core::x1::TargetIdentifier out;
        out.kind = kind_from(target.at("kind"));
        out.element = target.at("element");
        out.value = target.at("value");
        v.targets.push_back(std::move(out));
    }
    v.delivery = delivery_from(j.at("delivery"));
}

void to_json(nlohmann::json& j, const Destination& v) {
    j = {{"did", v.did},
         {"host", v.host},
         {"port", v.port},
         {"delivery", delivery_name(v.delivery)}};
}

void from_json(const nlohmann::json& j, Destination& v) {
    v.did = j.at("did");
    v.host = j.at("host");
    v.port = j.at("port").get<std::uint16_t>();
    v.delivery = delivery_from(j.at("delivery"));
}

std::optional<li_core::x1::ErrorCode> TaskStore::activate(const li_core::x1::TaskDetails& details) {
    if (details.xid.empty()) {
        return li_core::x1::ErrorCode::GenericError;
    }
    // TS 103 221-1 table 6.7-3: an XID that is already provisioned is error 2010, and it is the
    // store that knows, not the codec.
    if (redis_.sismember(kTaskIndex, details.xid)) {
        return li_core::x1::ErrorCode::XidAlreadyExists;
    }
    if (details.targets.empty()) {
        return li_core::x1::ErrorCode::GenericError;
    }
    Task task;
    task.xid = details.xid;
    task.routes = routes_from(details);
    if (task.routes.empty()) {
        // No LIID means nothing to label the delivered records with.
        return li_core::x1::ErrorCode::GenericError;
    }
    task.targets = details.targets;
    task.delivery = details.delivery;

    nlohmann::json j;
    to_json(j, task);
    redis_.set(kTaskPrefix + task.xid, j.dump());
    redis_.sadd(kTaskIndex, task.xid);
    return std::nullopt;
}

std::optional<li_core::x1::ErrorCode> TaskStore::modify(const li_core::x1::TaskDetails& details) {
    if (details.xid.empty() || !redis_.sismember(kTaskIndex, details.xid)) {
        return li_core::x1::ErrorCode::XidDoesNotExist;
    }
    // Annex C.2.2: after a ModifyTask "only the LIIDs included in the ModifyTask message ... remain
    // active", and any LIID that was associated with the task but is absent from the message
    // ceases. Replacing the stored routes wholesale is exactly that rule.
    Task task;
    task.xid = details.xid;
    task.routes = routes_from(details);
    if (task.routes.empty()) {
        return li_core::x1::ErrorCode::GenericError;
    }
    task.targets = details.targets;
    task.delivery = details.delivery;
    nlohmann::json j;
    to_json(j, task);
    redis_.set(kTaskPrefix + task.xid, j.dump());
    return std::nullopt;
}

std::optional<li_core::x1::ErrorCode> TaskStore::deactivate(const std::string& xid) {
    if (!redis_.sismember(kTaskIndex, xid)) {
        return li_core::x1::ErrorCode::XidDoesNotExist;
    }
    redis_.del(kTaskPrefix + xid);
    redis_.srem(kTaskIndex, xid);
    return std::nullopt;
}

std::optional<li_core::x1::ErrorCode> TaskStore::deactivate_all() {
    std::vector<std::string> xids;
    redis_.smembers(kTaskIndex, std::back_inserter(xids));
    for (const auto& xid : xids) {
        redis_.del(kTaskPrefix + xid);
    }
    redis_.del(kTaskIndex);
    return std::nullopt;
}

std::optional<li_core::x1::ErrorCode>
TaskStore::create_destination(const li_core::x1::DestinationDetails& details) {
    if (details.did.empty()) {
        return li_core::x1::ErrorCode::GenericError;
    }
    if (redis_.sismember(kDestIndex, details.did)) {
        return li_core::x1::ErrorCode::DidAlreadyExists;
    }
    if (details.address.kind != li_core::x1::DeliveryAddress::Kind::IpAddressAndPort) {
        // The other three DeliveryAddress kinds (E164, URI, email) describe handover to something
        // that is not a TS 102 232-1 TCP stream; this MDF2 delivers only over the latter.
        return li_core::x1::ErrorCode::GenericError;
    }
    Destination destination;
    destination.did = details.did;
    if (!split_host_port(details.address.value, destination.host, destination.port)) {
        return li_core::x1::ErrorCode::GenericError;
    }
    destination.delivery = details.delivery;
    nlohmann::json j;
    to_json(j, destination);
    redis_.set(kDestPrefix + destination.did, j.dump());
    redis_.sadd(kDestIndex, destination.did);
    return std::nullopt;
}

std::optional<li_core::x1::ErrorCode> TaskStore::remove_destination(const std::string& did) {
    if (!redis_.sismember(kDestIndex, did)) {
        return li_core::x1::ErrorCode::DidDoesNotExist;
    }
    redis_.del(kDestPrefix + did);
    redis_.srem(kDestIndex, did);
    return std::nullopt;
}

std::optional<li_core::x1::ErrorCode> TaskStore::remove_all_destinations() {
    std::vector<std::string> dids;
    redis_.smembers(kDestIndex, std::back_inserter(dids));
    for (const auto& did : dids) {
        redis_.del(kDestPrefix + did);
    }
    redis_.del(kDestIndex);
    return std::nullopt;
}

std::optional<Task> TaskStore::task(const std::string& xid) {
    const auto raw = redis_.get(kTaskPrefix + xid);
    if (!raw) {
        return std::nullopt;
    }
    Task out;
    from_json(nlohmann::json::parse(*raw), out);
    return out;
}

std::optional<Destination> TaskStore::destination(const std::string& did) {
    const auto raw = redis_.get(kDestPrefix + did);
    if (!raw) {
        return std::nullopt;
    }
    Destination out;
    from_json(nlohmann::json::parse(*raw), out);
    return out;
}

std::vector<Destination> TaskStore::destinations() {
    std::vector<std::string> dids;
    redis_.smembers(kDestIndex, std::back_inserter(dids));
    std::vector<Destination> out;
    out.reserve(dids.size());
    for (const auto& did : dids) {
        if (auto destination = this->destination(did)) {
            out.push_back(std::move(*destination));
        }
    }
    return out;
}

std::uint32_t TaskStore::next_sequence(const std::string& liid) {
    // INCR wraps at the 32-bit field's width: TS 102 232-1's sequenceNumber is
    // INTEGER (0..4294967295).
    const long long value = redis_.incr(kSeqPrefix + liid);
    return static_cast<std::uint32_t>(static_cast<unsigned long long>(value) & 0xFFFFFFFFULL);
}

std::vector<li_core::hi2::IriTargetIdentifier> lea_provided(const Task& task) {
    std::vector<li_core::hi2::IriTargetIdentifier> out;
    out.reserve(task.targets.size());
    for (const auto& target : task.targets) {
        // TS 33.128 clause 5.5.5: "For all Identifiers present in the provisioning message
        // received over X1, the MDF shall include the relevant Identifier with the provenance set
        // to lEAProvided."
        out.push_back(
            li_core::hi2::IriTargetIdentifier{target, li_core::hi2::Provenance::LeaProvided});
    }
    return out;
}

} // namespace li_mdf
