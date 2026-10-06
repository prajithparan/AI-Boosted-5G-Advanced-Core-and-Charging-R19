#include "lipf.hpp"

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <cstdio>

#include "hi1_service.hpp"

namespace li_admf {

namespace x1 = li_core::x1;

tl::expected<std::string, std::string> HttpX1Transport::post(const NetworkElement& ne,
                                                             const std::string& body) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = ne.x1_url;
    req.headers.emplace("content-type", "application/xml");
    req.body = body;
    auto resp = client_.send(req);
    if (!resp.has_value()) {
        return tl::make_unexpected("transport to " + ne.name + " failed: " + resp.error());
    }
    // X1 errors ride in a 200 body (TS 103 221-1 7.2.2.2); any other status is an HTTP-level fault.
    if (resp->status != 200) {
        return tl::make_unexpected(ne.name + " answered HTTP " + std::to_string(resp->status));
    }
    return resp->body;
}

std::string Lipf::destination_id(const std::string& xid, const std::string& address) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    const std::string material = xid + "|" + address;
    EVP_Digest(material.data(), material.size(), digest, &len, EVP_sha256(), nullptr);
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0F) | 0x40); // version 4 layout
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3F) | 0x80); // variant 10
    char buf[40];
    std::snprintf(buf,
                  sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6],
                  digest[7], digest[8], digest[9], digest[10], digest[11], digest[12], digest[13],
                  digest[14], digest[15]);
    return buf;
}

namespace {

bool supported_target(x1::TargetIdentifierKind kind) {
    // The identifiers the AMF IRI-POI matches a registering UE's SUPI against.
    return kind == x1::TargetIdentifierKind::SupiImsi || kind == x1::TargetIdentifierKind::SupiNai ||
           kind == x1::TargetIdentifierKind::Imsi || kind == x1::TargetIdentifierKind::Nai;
}

} // namespace

std::optional<std::string> Lipf::infeasible(const TaskSpec& spec) const {
    if (spec.targets.empty()) {
        return "the task names no target identifier";
    }
    for (const auto& t : spec.targets) {
        if (!supported_target(t.kind)) {
            return "target identifier '" + t.element + "' cannot be matched by any POI in this deployment";
        }
    }
    if (spec.wants_cc && !config_.cc_capable) {
        return "the task requires content interception (CC) but no CC-POI is deployed";
    }
    if (!spec.wants_iri && !spec.wants_cc) {
        return "the task requests neither IRI nor CC";
    }
    if (spec.destinations.empty()) {
        return "the task has no delivery destination";
    }
    bool have_poi = false;
    bool have_mdf = false;
    for (const auto& ne : elements_) {
        have_poi = have_poi || ne.role == "poi";
        have_mdf = have_mdf || ne.role == "mdf2";
    }
    if (!have_poi || !have_mdf) {
        return "no POI and MDF2 are configured to provision";
    }
    return std::nullopt;
}

x1::TaskDetails Lipf::mdf2_task(const TaskSpec& spec) const {
    x1::TaskDetails t;
    t.xid = spec.xid;
    t.targets = spec.targets;
    t.delivery = spec.wants_cc ? x1::DeliveryType::X2AndX3 : x1::DeliveryType::X2Only;
    std::vector<std::string> dids;
    for (const auto& d : spec.destinations) {
        dids.push_back(destination_id(spec.xid, d.address));
    }
    t.dids = dids;
    x1::MediationDetails md;
    md.liid = spec.liid;
    md.delivery = spec.wants_cc ? x1::MediationDeliveryType::Hi2AndHi3 : x1::MediationDeliveryType::Hi2Only;
    md.start_time = spec.start_time;
    md.end_time = spec.end_time;
    md.dids = dids;
    t.mediation_details.push_back(md);
    return t;
}

x1::TaskDetails Lipf::poi_task(const TaskSpec& spec) const {
    x1::TaskDetails t;
    t.xid = spec.xid;
    t.targets = spec.targets;
    t.delivery = x1::DeliveryType::X2Only; // an IRI-POI streams over X2; CC would be X3 (no CC-POI yet)
    // The POI's own X2 destination is its configured MDF2 (the POI ignores Destination routing,
    // ADR-0377), so no DIDs are provisioned on it.
    t.identifier_association_events = config_.poi_identifier_association;
    return t;
}

LipfResult Lipf::send(const NetworkElement& ne, x1::MessageType type, x1::RequestBody body) {
    x1::Request request;
    request.header = {config_.admf_identifier,
                      ne.ne_identifier,
                      now_microsecond_timestamp(),
                      config_.x1_version,
                      new_uuid()};
    request.type = type;
    request.body = std::move(body);
    const auto xml = x1::serialise_request({request});
    if (!xml) {
        return {false, "could not build the X1 request: " + xml.error(), std::nullopt};
    }
    const auto answer = transport_.post(ne, *xml);
    if (!answer) {
        return {false, answer.error(), std::nullopt};
    }
    const auto parsed = x1::parse_response(*answer);
    if (!parsed) {
        return {false, ne.name + " sent an unreadable X1 response: " + parsed.error().detail, std::nullopt};
    }
    if (parsed->size() != 1) {
        return {false, ne.name + " answered " + std::to_string(parsed->size()) + " responses to one request", std::nullopt};
    }
    if (const auto* err = std::get_if<x1::ErrorResponse>(&(*parsed)[0])) {
        return {false,
                ne.name + ": X1 error " + std::to_string(static_cast<int>(err->code)) + " " + err->description,
                static_cast<int>(err->code)};
    }
    return {true, "", std::nullopt};
}

LipfResult Lipf::provision(const TaskSpec& spec) {
    if (const auto why = infeasible(spec)) {
        return {false, *why, std::nullopt};
    }
    std::vector<const NetworkElement*> created_destinations_on;
    std::vector<const NetworkElement*> activated_on;
    const auto roll_back = [&] {
        for (const auto* ne : activated_on) {
            (void)send(*ne, x1::MessageType::DeactivateTask, x1::DeactivateTask{spec.xid});
        }
        for (const auto* ne : created_destinations_on) {
            for (const auto& d : spec.destinations) {
                (void)send(*ne, x1::MessageType::RemoveDestination,
                           x1::RemoveDestination{destination_id(spec.xid, d.address)});
            }
        }
    };
    const auto fail = [&](const LipfResult& r) {
        roll_back();
        return r;
    };

    // 1. MDF2: where the records go, then the task that maps XID -> LIID -> those destinations.
    for (const auto& ne : elements_) {
        if (ne.role != "mdf2") {
            continue;
        }
        created_destinations_on.push_back(&ne);
        for (const auto& d : spec.destinations) {
            x1::DestinationDetails dest;
            dest.did = destination_id(spec.xid, d.address);
            dest.delivery = spec.wants_cc ? x1::DeliveryType::X2AndX3 : x1::DeliveryType::X2Only;
            dest.address = {x1::DeliveryAddress::Kind::IpAddressAndPort, d.address};
            auto r = send(ne, x1::MessageType::CreateDestination, x1::CreateDestination{dest});
            // 2030 DidAlreadyExists: a retry of an earlier provisioning; the destination is already right.
            if (!r.ok && r.x1_error != static_cast<int>(x1::ErrorCode::DidAlreadyExists)) {
                return fail(r);
            }
        }
        auto r = send(ne, x1::MessageType::ActivateTask, x1::ActivateTask{mdf2_task(spec)});
        if (!r.ok && r.x1_error == static_cast<int>(x1::ErrorCode::XidAlreadyExists)) {
            r = send(ne, x1::MessageType::ModifyTask, x1::ModifyTask{mdf2_task(spec)}); // converge
        }
        if (!r.ok) {
            return fail(r);
        }
        activated_on.push_back(&ne);
    }
    // 2. Every POI: the target identifiers to intercept.
    for (const auto& ne : elements_) {
        if (ne.role != "poi") {
            continue;
        }
        auto r = send(ne, x1::MessageType::ActivateTask, x1::ActivateTask{poi_task(spec)});
        if (!r.ok && r.x1_error == static_cast<int>(x1::ErrorCode::XidAlreadyExists)) {
            r = send(ne, x1::MessageType::ModifyTask, x1::ModifyTask{poi_task(spec)});
        }
        if (!r.ok) {
            return fail(r);
        }
        activated_on.push_back(&ne);
    }
    return {true, "", std::nullopt};
}

LipfResult Lipf::deprovision(const TaskSpec& spec) {
    LipfResult worst{true, "", std::nullopt};
    for (const auto& ne : elements_) {
        auto r = send(ne, x1::MessageType::DeactivateTask, x1::DeactivateTask{spec.xid});
        if (!r.ok && r.x1_error == static_cast<int>(x1::ErrorCode::XidDoesNotExist)) {
            r = {true, "", std::nullopt}; // already gone: that is the goal
        }
        if (!r.ok && worst.ok) {
            worst = r; // keep going: leave no NE interception running because another NE failed
        }
    }
    for (const auto& ne : elements_) {
        if (ne.role != "mdf2") {
            continue;
        }
        for (const auto& d : spec.destinations) {
            // Best effort: a destination shared with another task is "in use" (7010) and stays.
            (void)send(ne, x1::MessageType::RemoveDestination,
                       x1::RemoveDestination{destination_id(spec.xid, d.address)});
        }
    }
    return worst;
}

std::vector<Lipf::KeepaliveResult> Lipf::keepalive_all() {
    std::vector<KeepaliveResult> out;
    for (const auto& ne : elements_) {
        const auto r = send(ne, x1::MessageType::Keepalive, x1::Keepalive{});
        out.push_back({ne.name, r.ok, r.detail});
    }
    return out;
}

void Lipf::retire_destinations(const std::string& xid, const std::vector<std::string>& addresses) {
    for (const auto& ne : elements_) {
        if (ne.role != "mdf2") {
            continue;
        }
        for (const auto& address : addresses) {
            (void)send(ne, x1::MessageType::RemoveDestination, x1::RemoveDestination{destination_id(xid, address)});
        }
    }
}

LipfResult Lipf::change_delivery(const TaskSpec& spec) {
    if (const auto why = infeasible(spec)) {
        return {false, *why, std::nullopt};
    }
    for (const auto& ne : elements_) {
        if (ne.role != "mdf2") {
            continue;
        }
        for (const auto& d : spec.destinations) {
            x1::DestinationDetails dest;
            dest.did = destination_id(spec.xid, d.address);
            dest.delivery = spec.wants_cc ? x1::DeliveryType::X2AndX3 : x1::DeliveryType::X2Only;
            dest.address = {x1::DeliveryAddress::Kind::IpAddressAndPort, d.address};
            auto r = send(ne, x1::MessageType::CreateDestination, x1::CreateDestination{dest});
            if (!r.ok && r.x1_error != static_cast<int>(x1::ErrorCode::DidAlreadyExists)) {
                return r;
            }
        }
        const auto r = send(ne, x1::MessageType::ModifyTask, x1::ModifyTask{mdf2_task(spec)});
        if (!r.ok) {
            return r;
        }
    }
    return {true, "", std::nullopt};
}

} // namespace li_admf
