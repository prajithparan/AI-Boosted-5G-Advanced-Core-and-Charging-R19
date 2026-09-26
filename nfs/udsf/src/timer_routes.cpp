// Nudsf_Timer -- TS 29.598 V19.5.0 clause 5.3 (procedures) and 6.2 (API), shapes from
// specs/5G_APIs-REL-19/TS29598_Nudsf_Timer.yaml (REL-19, commit bca84b6). ADR-0400/ADR-0402.
//
// A stored timer is the Timer as received (timerId stripped: "shall be absent" outside
// notifications) plus the schedule: next_ms (next expiry, 0 = none pending) and left (periodic
// repetitions still to come).
#include "sbi_core/datetime.hpp"

#include <chrono>

#include "TS26510_CommonData_grp.hpp"
#include "TS29598_Nudsf_Timer.hpp"
#include "patch.hpp"
#include "routes.hpp"
#include "search.hpp"

namespace udsf {

namespace {

using json = nlohmann::json;

Response empty(int status) {
    Response r;
    r.status = status;
    return r;
}

Response json_response(int status, const json& body) {
    Response r;
    r.status = status;
    r.headers.emplace("content-type", "application/json");
    r.body = body.dump();
    return r;
}

std::int64_t to_ms(const std::string& rfc3339) {
    const auto tp = sbi_core::parse_rfc3339(rfc3339);
    if (!tp) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp->time_since_epoch()).count();
}

// Shape + TS rules of a Timer body. Empty string = valid.
std::string validate_timer(const json& body, bool allow_timer_id) {
    sbi_gen::Timer t;
    try {
        t = body.get<sbi_gen::Timer>();
    } catch (const std::exception& e) {
        return std::string("invalid Timer: ") + e.what();
    }
    if (t.timerId && !allow_timer_id) {
        return "timerId shall be absent outside notifications (TS 29.598 table 6.2.6.2.2-1)";
    }
    if (to_ms(t.expires) == 0) {
        return "expires shall be a DateTime (RFC 3339)";
    }
    if (t.metaTags) {
        if (auto why = validate_tag_map(*t.metaTags, "Timer.metaTags"); !why.empty()) {
            return why;
        }
    }
    if (t.periodicRepetition) {
        if (*t.periodicRepetition <= 0) {
            return "periodicRepetition shall be a positive DurationSec";
        }
        if (!t.repetitionCount || *t.repetitionCount < 1) {
            return "repetitionCount (>= 1) shall be present with periodicRepetition";
        }
    }
    if (t.deleteAfter && *t.deleteAfter < 0) {
        return "deleteAfter shall be a Uinteger";
    }
    return "";
}

// (Re)derives the schedule from the body: first expiry at `expires`, `repetitionCount` more.
void schedule_from_body(Doc& d) {
    d.next_ms = to_ms(d.body.at("expires").get<std::string>());
    d.left = d.body.contains("periodicRepetition")
                 ? d.body.value("repetitionCount", std::int64_t{0})
                 : 0;
}

bool expired(const Doc& d, std::int64_t now) {
    return d.next_ms == 0 || d.next_ms <= now;
}

} // namespace

std::int64_t timer_delete_at_ms(const Doc& t) {
    if (!t.body.contains("deleteAfter")) {
        return 0; // deleted right after the (last) expiry, by the expiry worker
    }
    // The (last) expires time: the pending expiry plus the repetitions still to come, or -- once
    // nothing is pending -- the expires the timer last had.
    std::int64_t last = t.next_ms;
    if (last > 0) {
        last += t.left * t.body.value("periodicRepetition", std::int64_t{0}) * 1000;
    } else {
        last = to_ms(t.body.value("lastExpiry", t.body.at("expires").get<std::string>()));
    }
    return last + t.body.at("deleteAfter").get<std::int64_t>() * 1000;
}

void add_timer_routes(sbi_core::http2::Server& server, Ctx& ctx) {
    const std::string root = kTimerRoot;
    const std::string api_uri = root;
    const std::string scope = "nudsf-timer";
    const std::string timers = root + "/{realmId}/{storageId}/timers";
    const std::string one = timers + "/{timerId}";

    auto pre = [&ctx, api_uri, scope](const Request& req) -> tl::expected<StorageRef, Response> {
        if (auto deny = authorize(ctx, req, scope, api_uri)) {
            return tl::unexpected(*deny);
        }
        return resolve_storage(ctx, req);
    };

    // filter and/or expired-filter (6.2.3.2.3.1): the result matches both when both are given.
    auto select = [&ctx](const Request& req,
                         const StorageRef& s) -> tl::expected<std::set<std::string>, Response> {
        const auto filter_text = query(req, "filter");
        const bool expired_filter = query(req, "expired-filter").has_value();
        if (!filter_text && !expired_filter) {
            return tl::unexpected(problem(400,
                                          "Bad Request",
                                          "filter or expired-filter shall be present",
                                          std::string("MANDATORY_QUERY_PARAM_MISSING")));
        }
        auto index = ctx.store.timer_index(s);
        std::set<std::string> ids;
        if (filter_text) {
            auto expr = parse_filter_param(*filter_text);
            if (!expr) {
                return tl::unexpected(
                    problem(400, "Bad Request", expr.error(), std::string("INVALID_QUERY_PARAM")));
            }
            if (uses_id_list(*expr)) {
                return tl::unexpected(problem(400,
                                              "Bad Request",
                                              "a RecordIdList does not select timers",
                                              std::string("INVALID_QUERY_PARAM")));
            }
            auto m = evaluate(*expr, *index);
            if (!m) {
                return tl::unexpected(
                    problem(400, "Bad Request", m.error(), std::string("INVALID_QUERY_PARAM")));
            }
            ids = std::move(*m);
        } else {
            ids = index->all();
        }
        if (expired_filter) {
            const auto now = now_ms();
            std::set<std::string> kept;
            for (const auto& id : ids) {
                if (auto d = ctx.store.get_timer(s, id); d && expired(*d, now)) {
                    kept.insert(id);
                }
            }
            ids = std::move(kept);
        }
        return ids;
    };

    // ---- SearchTimer (5.3.2.5.2 expired, 5.3.2.5.3 tagged) --------------------------------------
    server.add_route("GET", timers, instrument("SearchTimer", [pre, select](const Request& req) {
                         auto s = pre(req);
                         if (!s) {
                             return s.error();
                         }
                         auto ids = select(req, *s);
                         if (!ids) {
                             return ids.error();
                         }
                         if (ids->empty()) {
                             return empty(204);
                         }
                         sbi_gen::TimerIdList out;
                         out.timerIds.assign(ids->begin(), ids->end());
                         return json_response(200, json(out));
                     }));

    // ---- DeleteTimers (5.3.2.4.3)
    // ----------------------------------------------------------------
    server.add_route(
        "DELETE", timers, instrument("DeleteTimers", [&ctx, pre, select](const Request& req) {
            auto s = pre(req);
            if (!s) {
                return s.error();
            }
            auto ids = select(req, *s);
            if (!ids) {
                return ids.error();
            }
            std::vector<std::string> deleted;
            std::vector<std::string> failed;
            for (const auto& id : *ids) {
                try {
                    auto tx = ctx.store.modify_timer(
                        *s,
                        id,
                        [](const std::optional<Doc>& b, Doc&) {
                            return b ? Verdict::Delete : Verdict::Abort;
                        },
                        timer_delete_at_ms);
                    if (tx.committed) {
                        deleted.push_back(id);
                    }
                } catch (const std::exception&) {
                    failed.push_back(id);
                }
            }
            if (deleted.empty() && failed.empty()) {
                return empty(204);
            }
            if (!failed.empty() &&
                (deleted.empty() || !feature_negotiated(query(req, "supported-features"), 3))) {
                return problem(
                    500,
                    "Internal Server Error",
                    std::to_string(failed.size()) +
                        " matching timer(s) could not be "
                        "stopped; negotiate TimerDeletePartialSuccess for a partial report",
                    std::string("SYSTEM_FAILURE"));
            }
            sbi_gen::TimerDeleteResponse out;
            out.timerIds = deleted;
            if (!failed.empty()) {
                out.failedTimerIdList = failed;
            }
            return json_response(200, json(out));
        }));

    // ---- CreateOrModifyTimer (5.3.2.2.2 Timer Start)
    // ---------------------------------------------
    server.add_route(
        "PUT", one, instrument("CreateOrModifyTimer", [&ctx, pre](const Request& req) {
            auto s = pre(req);
            if (!s) {
                return s.error();
            }
            const auto id = req.path_params.at("timerId");
            json body;
            try {
                body = json::parse(req.body);
            } catch (const json::parse_error& e) {
                return problem(400, "Malformed JSON", e.what(), std::string("INVALID_MSG_FORMAT"));
            }
            if (auto why = validate_timer(body, false); !why.empty()) {
                return problem(400, "Bad Request", why, std::string("INVALID_MSG_FORMAT"));
            }
            if (to_ms(body.at("expires").get<std::string>()) <= now_ms()) {
                return problem(403,
                               "Forbidden",
                               "the expires time is in the past",
                               std::string("EXPIRES_VALUE_NOT_ALLOWED"));
            }
            auto tx = ctx.store.modify_timer(
                *s,
                id,
                [&](const std::optional<Doc>&, Doc& next) {
                    next.body = body;
                    schedule_from_body(next);
                    return Verdict::Write;
                },
                timer_delete_at_ms);
            if (!tx.before) {
                Response r = empty(201);
                r.headers.emplace("location",
                                  ctx.settings.self_base + kTimerRoot + "/" + s->realm + "/" +
                                      s->storage + "/timers/" + id);
                return r;
            }
            return empty(204);
        }));

    // ---- UpdateTimer (5.3.2.3.2)
    // -------------------------------------------------------------------
    server.add_route(
        "PATCH", one, instrument("UpdateTimer", [&ctx, pre](const Request& req) {
            auto s = pre(req);
            if (!s) {
                return s.error();
            }
            const auto id = req.path_params.at("timerId");
            auto items = parse_patch_items(req.body);
            if (!items) {
                return problem(
                    400, "Bad Request", items.error(), std::string("INVALID_MSG_FORMAT"));
            }
            std::optional<Response> early;
            std::vector<sbi_gen::ReportItem> discarded;
            ctx.store.modify_timer(
                *s,
                id,
                [&](const std::optional<Doc>& before, Doc& next) {
                    early.reset();
                    discarded.clear();
                    if (!before) {
                        early = problem(404,
                                        "Not Found",
                                        "timer " + id + " does not exist",
                                        std::string("TIMER_NOT_FOUND"));
                        return Verdict::Abort;
                    }
                    json doc = before->body;
                    doc.erase("lastExpiry");
                    const auto now = now_ms();
                    auto res = apply_itemwise(doc, *items, [&](const json& d) {
                        auto why = validate_timer(d, false);
                        if (why.empty() && d.at("expires") != doc.at("expires") &&
                            to_ms(d.at("expires").get<std::string>()) <= now) {
                            why = "EXPIRES_VALUE_NOT_ALLOWED: the expires time is in the past";
                        }
                        return why;
                    });
                    discarded = res.discarded;
                    if (res.discarded.size() == items->size()) {
                        return Verdict::Abort;
                    }
                    const bool reschedule = res.document.at("expires") != doc.at("expires") ||
                                            res.document.value("periodicRepetition", json()) !=
                                                doc.value("periodicRepetition", json()) ||
                                            res.document.value("repetitionCount", json()) !=
                                                doc.value("repetitionCount", json());
                    next.body = res.document;
                    if (reschedule) {
                        schedule_from_body(next);
                    } else if (before->body.contains("lastExpiry")) {
                        next.body["lastExpiry"] = before->body["lastExpiry"];
                    }
                    return Verdict::Write;
                },
                timer_delete_at_ms);
            if (early) {
                return *early;
            }
            if (!discarded.empty()) {
                sbi_gen::PatchResult pr;
                pr.report = discarded;
                return json_response(200, json(pr));
            }
            return empty(204);
        }));

    // ---- DeleteTimer (5.3.2.4.2)
    // -------------------------------------------------------------------
    server.add_route("DELETE", one, instrument("DeleteTimer", [&ctx, pre](const Request& req) {
                         auto s = pre(req);
                         if (!s) {
                             return s.error();
                         }
                         const auto id = req.path_params.at("timerId");
                         auto tx = ctx.store.modify_timer(
                             *s,
                             id,
                             [](const std::optional<Doc>& b, Doc&) {
                                 return b ? Verdict::Delete : Verdict::Abort;
                             },
                             timer_delete_at_ms);
                         if (!tx.committed) {
                             return problem(404,
                                            "Not Found",
                                            "timer " + id + " does not exist",
                                            std::string("TIMER_NOT_FOUND"));
                         }
                         return empty(204);
                     }));

    // ---- GetTimer
    // ----------------------------------------------------------------------------------
    server.add_route("GET", one, instrument("GetTimer", [&ctx, pre](const Request& req) {
                         auto s = pre(req);
                         if (!s) {
                             return s.error();
                         }
                         const auto id = req.path_params.at("timerId");
                         auto d = ctx.store.get_timer(*s, id);
                         if (!d) {
                             return problem(404,
                                            "Not Found",
                                            "timer " + id + " does not exist",
                                            std::string("TIMER_NOT_FOUND"));
                         }
                         json body = d->body;
                         body.erase("lastExpiry");
                         return json_response(200, body);
                     }));
}

} // namespace udsf
