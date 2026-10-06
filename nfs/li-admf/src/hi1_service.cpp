#include "hi1_service.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <random>
#include <set>

namespace li_admf {

namespace hi1 = li_core::hi1;

std::string new_uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL; // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL; // variant 10
    char buf[40];
    std::snprintf(buf,
                  sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(hi >> 32),
                  static_cast<unsigned>((hi >> 16) & 0xFFFF),
                  static_cast<unsigned>(hi & 0xFFFF),
                  static_cast<unsigned>(lo >> 48),
                  static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFULL));
    return buf;
}

namespace {

std::string format_utc(std::chrono::system_clock::time_point tp, bool micros) {
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::string out = buf;
    if (micros) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(tp - secs).count();
        char frac[16];
        std::snprintf(frac, sizeof(frac), ".%06lld", static_cast<long long>(us));
        out += frac;
    }
    return out + "Z";
}

} // namespace

std::string now_microsecond_timestamp() {
    return format_utc(std::chrono::system_clock::now(), true);
}
std::string now_timestamp() {
    return format_utc(std::chrono::system_clock::now(), false);
}

Hi1Service::Hi1Service(Hi1Config config, Lifecycle* lifecycle)
    : config_(std::move(config)), config_last_changed_(now_timestamp()), lifecycle_(lifecycle) {}

namespace {

// The response header: the request's header with sender/receiver swapped and the SAME transaction
// identifier (6.2.5), our national profile, and the request's ETSIVersion (the version we are
// answering in is the one the sender asked for, once it has been accepted).
hi1::Header response_header(const Hi1Config& cfg, const hi1::Header& request) {
    hi1::Header h;
    h.sender = cfg.self;
    h.receiver = request.sender;
    h.transaction_id = request.transaction_id;
    h.timestamp = now_microsecond_timestamp();
    h.version = {
        request.version.etsi_version, cfg.national_profile_owner, cfg.national_profile_version};
    return h;
}

// A header for a failure where the request's own header could not be read (6.2.5: "assign the
// Response message a new unique Transaction Identifier").
hi1::Header fallback_header(const Hi1Config& cfg, const hi1::EndpointId& receiver) {
    hi1::Header h;
    h.sender = cfg.self;
    h.receiver = receiver;
    h.transaction_id = new_uuid();
    h.timestamp = now_microsecond_timestamp();
    h.version = {cfg.supported_etsi_versions.empty() ? "V1.0.0"
                                                     : cfg.supported_etsi_versions.front(),
                 cfg.national_profile_owner,
                 cfg.national_profile_version};
    return h;
}

std::string
top_level_failure(const hi1::Header& header, hi1::ErrorCode code, const std::string& why) {
    hi1::Response r;
    r.header = header;
    r.payload = hi1::Failure{static_cast<std::uint32_t>(code), why};
    auto xml = hi1::serialise_response(r);
    if (!xml) {
        // The only way this fails is a description the schema refuses (e.g. over-long); keep the
        // interface answering with a bare, schema-safe reason rather than an empty body.
        r.payload = hi1::Failure{static_cast<std::uint32_t>(code), "request rejected"};
        xml = hi1::serialise_response(r);
    }
    return xml ? *xml : std::string();
}

// A top-level rejection: the body plus the audit fields.
Hi1Reply
rejection(Hi1Reply reply, const hi1::Header& header, hi1::ErrorCode code, const std::string& why) {
    reply.body = top_level_failure(header, code, why);
    reply.outcome = "rejected";
    reply.detail = std::to_string(static_cast<std::uint32_t>(code)) + ": " + why;
    return reply;
}

const LeaBinding* binding_for(const Hi1Config& cfg, std::string_view cn) {
    for (const auto& b : cfg.leas) {
        if (b.peer_cert_cn == cn) {
            return &b;
        }
    }
    return nullptr;
}

// The ETSI-defined LI workflow endpoint dictionary values (ts_103120_ETSIDictionaryDefinitions.xml,
// LIWorkflowEndpoint) and the default relative URL of each (TS 103 120 table H.0b).
struct WorkflowPath {
    const char* value;
    const char* path;
    const char* guidance;
};
constexpr WorkflowPath kLiWorkflows[] = {
    {"NewAuthorisation",
     "/li/authorisation/new",
     "Serve a new LI authorisation with its tasks and documents"},
    {"AuthorisationExtension",
     "/li/authorisation/extension",
     "Extend the end date of an authorisation"},
    {"AuthorisationCancellation", "/li/authorisation/cancellation", "Cancel an authorisation"},
    {"TaskAddition", "/li/task/addition", "Add LI tasks to an existing authorisation"},
    {"TaskCancellation", "/li/task/cancellation", "Cancel LI tasks"},
    {"ChangeOfDelivery", "/li/task/change-delivery", "Change a task's delivery endpoint"},
};

hi1::CspConfig build_csp_config(const Hi1Config& cfg, const std::string& last_changed) {
    hi1::CspConfig c;
    c.last_changed = last_changed;
    // Only formats ETSI itself defines (Annex C); the target identifiers this CSP can action are
    // the subscriber identities the POIs match on (AMF POI: SUPI/IMSI/NAI kinds).
    for (const char* format : {"SUPIIMSI", "SUPINAI", "IMSI", "NAI"}) {
        c.targeting.push_back({format, "ETSI", std::nullopt, {}});
    }
    for (const auto& w : kLiWorkflows) {
        c.li_endpoints.push_back(
            {{"ETSI", "LIWorkflowEndpoint", w.value}, w.guidance, cfg.public_base_url + w.path});
    }
    return c;
}

} // namespace

Hi1Reply Hi1Service::handle(std::string_view peer_cert_cn,
                            std::string_view path,
                            std::string_view content_type,
                            const std::string& body) const {
    Hi1Reply reply;
    const LeaBinding* lea = binding_for(config_, peer_cert_cn);
    if (lea == nullptr) {
        // Transport-level authentication failure (9.3.4), not an HI1 application error: an
        // unknown client certificate never reaches the message layer.
        spdlog::warn("li-admf: HI1 request from unbound mTLS peer '{}' refused", peer_cert_cn);
        reply.http_status = 403;
        reply.detail = "no LEA binding for this client certificate";
        return reply;
    }

    // 9.2.0: only the XML encoding is implemented (ADR-0462 decision 1). A JSON request cannot even
    // be parsed for a header, so the answer carries a fresh transaction id (6.2.5).
    if (content_type.find("xml") == std::string_view::npos) {
        return rejection(reply,
                         fallback_header(config_, lea->endpoint),
                         hi1::ErrorCode::UnsupportedEncoding,
                         "only the XML encoding (text/xml) is supported");
    }

    auto parsed = hi1::parse_request(body);
    if (!parsed) {
        const auto& err = parsed.error();
        reply.transaction_id = err.header ? err.header->transaction_id : "";
        return rejection(reply,
                         err.header ? response_header(config_, *err.header)
                                    : fallback_header(config_, lea->endpoint),
                         err.code,
                         err.detail);
    }
    const hi1::Request& req = *parsed;
    const hi1::Header answer = response_header(config_, req.header);
    reply.transaction_id = req.header.transaction_id;
    reply.sender = req.header.sender.country_code + "/" + req.header.sender.unique_identifier;
    reply.actions = static_cast<int>(req.actions.size());

    // 6.2.3 / D.1 code 3021: "The application shall deliver the supported versions".
    const auto& versions = config_.supported_etsi_versions;
    if (std::find(versions.begin(), versions.end(), req.header.version.etsi_version) ==
        versions.end()) {
        std::string list;
        for (const auto& v : versions) {
            list += (list.empty() ? "" : ", ") + v;
        }
        return rejection(reply,
                         answer,
                         hi1::ErrorCode::VersionNotSupported,
                         "ETSIVersion " + req.header.version.etsi_version +
                             " is not supported; supported: " + list);
    }

    // Identities. The ReceiverIdentifier must be us; the SenderIdentifier must be the EndpointID
    // this mTLS peer is onboarded as. Annex D has no code for either; 3007 ("Improper value:
    // semantic value does not fit context") is the closest and is cited in the description.
    if (!(req.header.receiver == config_.self)) {
        return rejection(reply,
                         answer,
                         hi1::ErrorCode::ImproperValue,
                         "ReceiverIdentifier is not this endpoint");
    }
    if (!(req.header.sender == lea->endpoint)) {
        return rejection(
            reply,
            answer,
            hi1::ErrorCode::ImproperValue,
            "SenderIdentifier does not match the endpoint this client is onboarded as");
    }

    // 6.4.4: Action Identifiers start at zero and increase by one; anything else is rejected with a
    // top-level error before any action is performed. A repeated id is the duplicate case (3002).
    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < req.actions.size(); ++i) {
        const auto id = req.actions[i].id;
        if (!seen.insert(id).second) {
            return rejection(reply,
                             answer,
                             hi1::ErrorCode::DuplicateActionId,
                             "duplicate ActionIdentifier " + std::to_string(id));
        }
        if (id != i) {
            return rejection(reply,
                             answer,
                             hi1::ErrorCode::ImproperValue,
                             "ActionIdentifiers must start at 0 and increase by 1");
        }
    }

    const auto workflow = workflow_for_path(path);
    if (!workflow) {
        return rejection(reply,
                         answer,
                         hi1::ErrorCode::ImproperValue,
                         "no HI1 workflow endpoint at " + std::string(path));
    }
    const bool has_config =
        std::any_of(req.actions.begin(), req.actions.end(), [](const hi1::Action& a) {
            return std::holds_alternative<hi1::GetCspConfigAction>(a.body);
        });
    if (has_config && *workflow != Workflow::None) {
        return rejection(reply,
                         answer,
                         hi1::ErrorCode::ImproperValue,
                         "GETCSPCONFIG is taken at the API base URL, not at a workflow endpoint");
    }

    hi1::Response resp;
    resp.header = answer;
    std::vector<hi1::ActionResult> results;
    if (lifecycle_ == nullptr) {
        // No lifecycle engine: only GETCSPCONFIG is served; nothing else is silently accepted.
        for (const auto& action : req.actions) {
            hi1::ActionResult r;
            r.id = action.id;
            if (std::holds_alternative<hi1::GetCspConfigAction>(action.body)) {
                r.outcome = hi1::ConfigResult{build_csp_config(config_, config_last_changed_)};
            } else {
                r.outcome =
                    hi1::Failure{static_cast<std::uint32_t>(hi1::ErrorCode::FeatureNotSupported),
                                 "this action is not implemented (no lifecycle engine)"};
            }
            results.push_back(std::move(r));
        }
    } else {
        std::vector<hi1::Action> for_lifecycle;
        for (const auto& action : req.actions) {
            if (!std::holds_alternative<hi1::GetCspConfigAction>(action.body)) {
                for_lifecycle.push_back(action);
            }
        }
        Lifecycle::Outcome outcome;
        if (!for_lifecycle.empty() || *workflow != Workflow::None) {
            outcome = lifecycle_->handle(*workflow, lea->endpoint, req.header, for_lifecycle);
        }
        if (outcome.rejected) {
            return rejection(reply,
                             answer,
                             static_cast<hi1::ErrorCode>(outcome.rejected->code),
                             outcome.rejected->description);
        }
        std::map<std::uint64_t, hi1::ActionResult> by_id;
        for (auto& r : outcome.results) {
            by_id[r.id] = std::move(r);
        }
        for (const auto& action : req.actions) {
            if (std::holds_alternative<hi1::GetCspConfigAction>(action.body)) {
                results.push_back(
                    {action.id,
                     hi1::ConfigResult{build_csp_config(config_, config_last_changed_)}});
            } else {
                results.push_back(std::move(by_id.at(action.id)));
            }
        }
    }
    resp.payload = std::move(results);
    auto xml = hi1::serialise_response(resp);
    if (!xml) {
        spdlog::error("li-admf: could not build an HI1 response: {}", xml.error());
        Hi1Reply failed = rejection(reply,
                                    answer,
                                    hi1::ErrorCode::TransientTechnicalError,
                                    "the response could not be built");
        failed.outcome = "error";
        return failed;
    }
    reply.body = std::move(*xml);
    reply.outcome = "ok";
    return reply;
}

} // namespace li_admf
