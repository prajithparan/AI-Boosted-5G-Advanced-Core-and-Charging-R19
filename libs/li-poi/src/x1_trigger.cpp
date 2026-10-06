#include "li_poi/x1_trigger.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <random>

namespace li_poi {

namespace {

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

// TS 103 280 QualifiedMicrosecondDateTime: UTC, six fractional digits, trailing Z.
std::string now_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - secs).count();
    struct tm utc {};
    gmtime_r(&t, &utc);
    char buf[48];
    std::snprintf(buf,
                  sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%06lldZ",
                  utc.tm_year + 1900,
                  utc.tm_mon + 1,
                  utc.tm_mday,
                  utc.tm_hour,
                  utc.tm_min,
                  utc.tm_sec,
                  static_cast<long long>(micros));
    return buf;
}

} // namespace

X1Trigger::X1Trigger(TriggerConfig config, sbi_core::http2::TlsConfig tls)
    : config_(std::move(config)), client_(std::move(tls)) {}

tl::expected<void, std::string> X1Trigger::send(li_core::x1::RequestBody body) {
    li_core::x1::Request request;
    request.header = {config_.tf_identifier,
                      config_.ne_identifier,
                      now_timestamp(),
                      config_.x1_version,
                      new_uuid()};
    if (std::holds_alternative<li_core::x1::ActivateTask>(body)) {
        request.type = li_core::x1::MessageType::ActivateTask;
    } else if (std::holds_alternative<li_core::x1::ModifyTask>(body)) {
        request.type = li_core::x1::MessageType::ModifyTask;
    } else {
        request.type = li_core::x1::MessageType::DeactivateTask;
    }
    request.body = std::move(body);
    const auto xml = li_core::x1::serialise_request({request});
    if (!xml) {
        return tl::make_unexpected("could not build the X1 request: " + xml.error());
    }
    sbi_core::http2::ClientRequest http;
    http.method = "POST";
    http.url = config_.x1_url;
    http.headers.emplace("content-type", "application/xml");
    http.body = *xml;
    const auto response = client_.send(http);
    if (!response) {
        return tl::make_unexpected("X1 to " + config_.ne_identifier +
                                   " failed: " + response.error());
    }
    if (response->status != 200) {
        return tl::make_unexpected(config_.ne_identifier + " answered HTTP " +
                                   std::to_string(response->status));
    }
    const auto parsed = li_core::x1::parse_response(response->body);
    if (!parsed || parsed->size() != 1) {
        return tl::make_unexpected(config_.ne_identifier + " sent an unreadable X1 response");
    }
    if (const auto* error = std::get_if<li_core::x1::ErrorResponse>(&(*parsed)[0])) {
        return tl::make_unexpected(config_.ne_identifier + ": X1 error " +
                                   std::to_string(static_cast<int>(error->code)) + " " +
                                   error->description);
    }
    return {};
}

tl::expected<void, std::string>
X1Trigger::send_task(bool activate,
                     const std::string& xid,
                     const std::vector<li_core::x1::TargetIdentifier>& targets,
                     const std::vector<std::string>& dids) {
    li_core::x1::TaskDetails task;
    task.xid = xid;
    task.targets = targets;
    task.delivery = li_core::x1::DeliveryType::X3Only;
    task.dids = dids;
    if (activate) {
        return send(li_core::x1::ActivateTask{task});
    }
    return send(li_core::x1::ModifyTask{task});
}

tl::expected<void, std::string>
X1Trigger::activate(const std::string& xid,
                    const std::vector<li_core::x1::TargetIdentifier>& targets,
                    const std::vector<std::string>& dids) {
    return send_task(true, xid, targets, dids);
}

tl::expected<void, std::string>
X1Trigger::modify(const std::string& xid,
                  const std::vector<li_core::x1::TargetIdentifier>& targets,
                  const std::vector<std::string>& dids) {
    return send_task(false, xid, targets, dids);
}

tl::expected<void, std::string> X1Trigger::deactivate(const std::string& xid) {
    return send(li_core::x1::DeactivateTask{xid});
}

} // namespace li_poi
