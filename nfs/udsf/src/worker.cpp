#include "worker.hpp"

#include "sbi_core/datetime.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/multipart.hpp"
#include "sbi_core/sbi_headers.hpp"

#include <spdlog/spdlog.h>
#include <thread>

#include "TS29598_Nudsf_DataRepository.hpp"
#include "routes.hpp"

namespace udsf {

namespace {

using json = nlohmann::json;
namespace mp = sbi_core::multipart;

constexpr long kBatch = 256;

// 3gpp-Sbi-Callback values: TS 29.500 Annex B lists none for the UDSF; these follow its
// <API name>_<callback> pattern with the callback names of the two YAMLs (ADR-0402).
constexpr const char* kCbRecordExpired = "Nudsf_DataRepository_recordExpired";
constexpr const char* kCbDataChange = "Nudsf_DataRepository_onDataChange";
constexpr const char* kCbSubExpiry = "Nudsf_DataRepository_subscriptionExpiryNotification";
constexpr const char* kCbTimerExpiry = "Nudsf_Timer_timerExpiry";

json blocks_json(const Record& r) {
    json out = json::array();
    for (const auto& b : r.blocks) {
        out.push_back({{"id", b.id}, {"ct", b.content_type}, {"cte", b.cte}, {"b64", base64_encode(b.data)}});
    }
    return out;
}

void append_blocks(std::vector<mp::Part>& parts, const json& blocks) {
    for (const auto& b : blocks) {
        mp::Part p{b.at("ct").get<std::string>(), b.at("id").get<std::string>(),
                   base64_decode(b.at("b64").get<std::string>()).value_or("")};
        p.content_transfer_encoding = b.at("cte").get<std::string>();
        parts.push_back(std::move(p));
    }
}

std::int64_t to_ms(const std::string& rfc3339) {
    const auto tp = sbi_core::parse_rfc3339(rfc3339);
    return tp ? std::chrono::duration_cast<std::chrono::milliseconds>(tp->time_since_epoch()).count()
              : 0;
}

opentelemetry::metrics::Counter<std::uint64_t>& sent_counter() {
    static auto c = sbi_core::get_meter("udsf")->CreateUInt64Counter(
        "udsf_notifications_total", "UDSF notifications POSTed, by kind and outcome");
    return *c;
}

opentelemetry::metrics::Counter<std::uint64_t>& expired_counter() {
    static auto c = sbi_core::get_meter("udsf")->CreateUInt64Counter(
        "udsf_expiries_total", "Records, subscriptions and timers the UDSF expired, by kind");
    return *c;
}

} // namespace

Worker::Worker(Ctx& ctx, sbi_core::http2::Client& client, std::chrono::milliseconds interval)
    : ctx_(ctx), client_(client), interval_(interval) {}

void Worker::run() {
    while (!stop_) {
        try {
            tick();
        } catch (const std::exception& e) {
            spdlog::error("udsf: expiry/delivery pass failed: {}", e.what());
        }
        std::this_thread::sleep_for(interval_);
    }
}

int Worker::tick() {
    int delivered = 0;
    for (const auto& [realm, storage] : ctx_.settings.storages) {
        const StorageRef s{realm, storage};
        sweep(s, now_ms());
        delivered += deliver(s);
    }
    return delivered;
}

void Worker::sweep(const StorageRef& s, std::int64_t now) {
    Store& store = ctx_.store;
    // Records whose ttl passed (5.2.2.6.2): delete, then notify the meta's callbackReference.
    for (const auto& id : store.claim_due(s, "rec", now, kBatch)) {
        std::optional<Record> gone;
        auto tx = store.modify_record(s, id, [&](const std::optional<Record>& before, Record&) {
            if (!before) {
                return Verdict::Abort;
            }
            const auto ttl = before->meta.contains("ttl") && before->meta["ttl"].is_string()
                                 ? to_ms(before->meta["ttl"].get<std::string>())
                                 : 0;
            if (ttl == 0 || ttl > now) {
                return Verdict::Abort; // ttl moved or removed after it was scheduled
            }
            return Verdict::Delete;
        });
        if (!tx.committed) {
            if (tx.before && tx.before->meta.contains("ttl")) {
                const auto ttl = to_ms(tx.before->meta["ttl"].get<std::string>());
                if (ttl > now) {
                    store.schedule(s, "rec", id, ttl);
                }
            }
            continue;
        }
        expired_counter().Add(1, {{"kind", "record"}});
        const auto cb = tx.before->meta.value("callbackReference", std::string());
        if (!cb.empty()) {
            store.enqueue(s, json{{"kind", "recordExpired"},
                                  {"url", cb},
                                  {"location", store.record_uri(s, id)},
                                  {"meta", tx.before->meta},
                                  {"blocks", blocks_json(*tx.before)}});
        }
    }
    // Advance expiry notice (expiryNotification > 0 seconds before expiry).
    for (const auto& id : store.claim_due(s, "subnot", now, kBatch)) {
        if (auto d = store.get_sub(s, id); d && d->body.contains("expiryCallbackReference")) {
            store.enqueue(s, json{{"kind", "subscriptionExpiry"},
                                  {"url", d->body["expiryCallbackReference"]},
                                  {"info", json{{"expiredSubscriptions", json::array({d->body})}}}});
        }
    }
    // Subscriptions whose expiry passed: remove; notify now if expiryNotification was 0.
    for (const auto& id : store.claim_due(s, "subexp", now, kBatch)) {
        auto tx = store.modify_sub(
            s, id,
            [](const std::optional<Doc>& b, Doc&) { return b ? Verdict::Delete : Verdict::Abort; },
            [](const Doc&) { return std::pair<std::int64_t, std::int64_t>{0, 0}; });
        if (!tx.committed) {
            continue;
        }
        expired_counter().Add(1, {{"kind", "subscription"}});
        const auto& b = tx.before->body;
        if (b.contains("expiryCallbackReference") && b.contains("expiryNotification") &&
            b["expiryNotification"].get<std::int64_t>() == 0) {
            store.enqueue(s, json{{"kind", "subscriptionExpiry"},
                                  {"url", b["expiryCallbackReference"]},
                                  {"info", json{{"expiredSubscriptions", json::array({b})}}}});
        }
    }
    // Timers (5.3.2.6.2): notify, then repeat, keep until deleteAfter, or delete.
    for (const auto& id : store.claim_due(s, "tmr", now, kBatch)) {
        json fired;
        auto tx = store.modify_timer(
            s, id,
            [&](const std::optional<Doc>& before, Doc& next) {
                if (!before || before->next_ms == 0 || before->next_ms > now) {
                    return Verdict::Abort;
                }
                fired = before->body;
                fired.erase("lastExpiry");
                fired["expires"] = sbi_core::format_rfc3339(
                    std::chrono::system_clock::time_point(std::chrono::milliseconds(before->next_ms)));
                if (before->left > 0) {
                    next.left = before->left - 1;
                    next.next_ms = before->next_ms +
                                   before->body.value("periodicRepetition", std::int64_t{0}) * 1000;
                    return Verdict::Write;
                }
                if (before->body.contains("deleteAfter")) {
                    next.body["lastExpiry"] = fired["expires"];
                    next.next_ms = 0;
                    return Verdict::Write;
                }
                return Verdict::Delete;
            },
            timer_delete_at_ms);
        if (!tx.committed) {
            continue;
        }
        expired_counter().Add(1, {{"kind", "timer"}});
        if (fired.contains("callbackReference")) {
            const auto url = fired["callbackReference"];
            fired.erase("callbackReference"); // "Shall be absent from Notification request messages"
            fired["timerId"] = id;            // "shall be present in Notification request messages"
            store.enqueue(s, json{{"kind", "timerExpiry"}, {"url", url}, {"timer", fired}});
        }
    }
    for (const auto& id : store.claim_due(s, "tmrdel", now, kBatch)) {
        store.modify_timer(
            s, id,
            [&](const std::optional<Doc>& b, Doc&) {
                return b && b->next_ms == 0 ? Verdict::Delete : Verdict::Abort;
            },
            timer_delete_at_ms);
    }
}

int Worker::deliver(const StorageRef& s) {
    int delivered = 0;
    for (const auto& n : ctx_.store.dequeue(s, kBatch)) {
        const auto kind = n.value("kind", std::string());
        sbi_core::http2::ClientRequest req;
        req.method = "POST";
        req.url = n.value("url", std::string());
        if (kind == "recordExpired") {
            std::vector<mp::Part> parts{{"application/json", std::string("meta"), n["meta"].dump()}};
            append_blocks(parts, n["blocks"]);
            const auto enc = mp::encode_subtype("mixed", parts);
            req.headers.emplace("content-type", enc.content_type_header);
            req.headers.emplace("content-location", n.value("location", std::string()));
            req.headers.emplace(sbi_core::headers::kCallback, kCbRecordExpired);
            req.body = enc.body;
        } else if (kind == "onDataChange") {
            std::vector<mp::Part> parts{
                {"application/json", std::string("descriptor"), n["descriptor"].dump()},
                {"application/json", std::string("meta"), n["meta"].dump()}};
            append_blocks(parts, n["blocks"]);
            const auto enc = mp::encode_subtype("mixed", parts);
            req.headers.emplace("content-type", enc.content_type_header);
            req.headers.emplace(sbi_core::headers::kCallback, kCbDataChange);
            req.body = enc.body;
        } else if (kind == "subscriptionExpiry") {
            req.headers.emplace("content-type", "application/json");
            req.headers.emplace(sbi_core::headers::kCallback, kCbSubExpiry);
            req.body = n["info"].dump();
        } else if (kind == "timerExpiry") {
            req.headers.emplace("content-type", "application/json");
            req.headers.emplace(sbi_core::headers::kCallback, kCbTimerExpiry);
            req.body = n["timer"].dump();
        } else {
            continue;
        }
        auto resp = client_.send(req);
        const bool ok = resp && resp->status >= 200 && resp->status < 300;
        sent_counter().Add(1, {{"kind", kind}, {"outcome", ok ? "delivered" : "failed"}});
        if (ok) {
            ++delivered;
        } else {
            spdlog::warn("udsf: {} notification to {} failed ({})", kind, req.url,
                         resp ? std::to_string(resp->status) : resp.error());
        }
    }
    return delivered;
}

} // namespace udsf
