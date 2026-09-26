// Nudsf_DataRepository -- TS 29.598 V19.5.0 clause 5.2 (procedures) and 6.1 (API), shapes from
// specs/5G_APIs-REL-19/TS29598_Nudsf_DataRepository.yaml (API 1.3.0, REL-19, commit bca84b6).
// Decisions and every deviation: docs/DECISIONS.md ADR-0400..ADR-0402.
#include "sbi_core/datetime.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/otel.hpp"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

#include "TS26510_CommonData_grp.hpp"
#include "TS29598_Nudsf_DataRepository.hpp"
#include "patch.hpp"
#include "routes.hpp"
#include "search.hpp"

namespace udsf {

namespace {

using json = nlohmann::json;
namespace mp = sbi_core::multipart;

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

std::string dr_uri(const Ctx& ctx, const StorageRef& s, const std::string& tail) {
    return ctx.settings.self_base + kDrRoot + "/" + s.realm + "/" + s.storage + "/" + tail;
}

std::int64_t to_ms(const std::string& rfc3339) {
    const auto tp = sbi_core::parse_rfc3339(rfc3339);
    if (!tp) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp->time_since_epoch()).count();
}

std::string from_ms(std::int64_t ms) {
    return sbi_core::format_rfc3339(std::chrono::system_clock::time_point(std::chrono::milliseconds(ms)));
}

// Operator ttl ceiling (TS 29.598 6.1.3.3.3.2: "If due to operator's policy the value of the ttl
// ... exceeded a maximum allowed ttl value with the ttl set to the value applied by the UDSF").
// Returns true if the meta's ttl was lowered.
bool clamp_ttl(const Ctx& ctx, json& meta) {
    if (ctx.settings.max_record_ttl_seconds <= 0 || !meta.contains("ttl")) {
        return false;
    }
    const auto ceiling = now_ms() + ctx.settings.max_record_ttl_seconds * 1000;
    if (to_ms(meta["ttl"].get<std::string>()) <= ceiling) {
        return false;
    }
    meta["ttl"] = from_ms(ceiling);
    return true;
}

// A multipart/mixed Record request body (TS 29.598 6.1.2.4.2): first part the RecordMeta, then
// block parts, each identified by its Content-Id.
struct ParsedRecord {
    json meta;
    std::vector<Block> blocks;
};

tl::expected<ParsedRecord, Response> parse_record_body(const Request& req) {
    const auto ct = header(req, "content-type").value_or("");
    if (!mp::is_multipart(ct)) {
        return tl::unexpected(problem(415, "Unsupported Media Type",
                                      "a Record is sent as multipart/mixed (TS 29.598 6.1.2.4.2)"));
    }
    auto parts = mp::parse_any(ct, req.body);
    if (!parts) {
        return tl::unexpected(problem(400, "Malformed multipart body", parts.error(),
                                      std::string("INVALID_MSG_FORMAT")));
    }
    ParsedRecord out;
    try {
        out.meta = (*parts)[0].body.empty() ? json::object() : json::parse((*parts)[0].body);
    } catch (const json::parse_error& e) {
        return tl::unexpected(problem(400, "Malformed JSON in meta part", e.what(),
                                      std::string("INVALID_MSG_FORMAT")));
    }
    if (auto why = validate_record_meta(out.meta); !why.empty()) {
        return tl::unexpected(problem(400, "Bad Request", why, std::string("INVALID_MSG_FORMAT")));
    }
    for (std::size_t i = 1; i < parts->size(); ++i) {
        auto& p = (*parts)[i];
        if (!p.content_id || p.content_id->empty()) {
            return tl::unexpected(problem(400, "Bad Request",
                                          "every block part needs a Content-Id (its blockId)",
                                          std::string("MANDATORY_IE_MISSING")));
        }
        Block b;
        b.id = *p.content_id;
        b.content_type = p.content_type.empty() ? "application/octet-stream" : p.content_type;
        b.cte = p.content_transfer_encoding.value_or("binary");
        b.data = std::move(p.body);
        if (std::any_of(out.blocks.begin(), out.blocks.end(),
                        [&](const Block& o) { return o.id == b.id; })) {
            return tl::unexpected(problem(400, "Bad Request", "duplicate blockId " + b.id,
                                          std::string("INVALID_MSG_FORMAT")));
        }
        out.blocks.push_back(std::move(b));
    }
    return out;
}

Response block_response(const Ctx& ctx, int status, const Block& b, bool cacheable) {
    Response r;
    r.status = status;
    r.headers.emplace("content-type", b.content_type);
    if (!b.cte.empty() && b.cte != "binary") {
        r.headers.emplace("content-transfer-encoding", b.cte);
    }
    r.body = b.data;
    add_validators(ctx, r, b.etag, b.lm, cacheable);
    return r;
}

Response not_found(const std::string& what, const std::string& id, const char* cause) {
    return problem(404, "Not Found", what + " " + id + " does not exist", std::string(cause));
}

// ---- subscriptions -------------------------------------------------------------------------------

std::string client_key(const sbi_gen::ClientId& c) {
    return json(c).dump();
}

// Expiry (policy-capped) and the time the expiry notification is due. Mutates the subscription
// to what the UDSF applied (TS 29.598 table 6.1.6.2.10-1).
void apply_expiry_policy(const Ctx& ctx, sbi_gen::NotificationSubscription& sub) {
    const auto now = now_ms();
    if (ctx.settings.max_subscription_seconds > 0) {
        const auto ceiling = now + ctx.settings.max_subscription_seconds * 1000;
        if (!sub.expiry || to_ms(*sub.expiry) > ceiling) {
            sub.expiry = from_ms(ceiling);
        }
    }
    if (sub.expiryNotification && sub.expiry) {
        const auto notify_at = to_ms(*sub.expiry) - *sub.expiryNotification * 1000;
        if (notify_at <= now) {
            sub.expiryNotification = 0; // "treated the same as a value of 0"
        }
    }
}

std::pair<std::int64_t, std::int64_t> sub_due(const Doc& d) {
    sbi_gen::NotificationSubscription sub;
    try {
        sub = d.body.get<sbi_gen::NotificationSubscription>();
    } catch (const std::exception&) {
        return {0, 0};
    }
    if (!sub.expiry) {
        return {0, 0};
    }
    const auto exp = to_ms(*sub.expiry);
    const std::int64_t notify =
        sub.expiryNotification && *sub.expiryNotification > 0 ? exp - *sub.expiryNotification * 1000
                                                               : 0;
    return {exp, notify};
}

std::string validate_subscription(const json& body, sbi_gen::NotificationSubscription* out) {
    sbi_gen::NotificationSubscription sub;
    try {
        sub = body.get<sbi_gen::NotificationSubscription>();
    } catch (const std::exception& e) {
        return std::string("invalid NotificationSubscription: ") + e.what();
    }
    if (!sub.clientId.nfId && !sub.clientId.nfSetId) {
        return "clientId shall contain nfId or nfSetId (TS 29.598 table 6.1.6.2.14-1)";
    }
    if (sub.expiryNotification && !sub.expiryCallbackReference) {
        return "expiryCallbackReference shall be present if expiryNotification is present";
    }
    if (sub.expiry && to_ms(*sub.expiry) == 0) {
        return "expiry shall be a DateTime";
    }
    if (sub.subFilter && sub.subFilter->operations && sub.subFilter->operations->size() > 3) {
        return "subFilter.operations has at most 3 items";
    }
    if (out != nullptr) {
        *out = std::move(sub);
    }
    return "";
}

// The body a NotificationSubscription response carries: what was stored, plus the negotiated
// supportedFeatures.
json sub_body(const Doc& d, const std::optional<std::string>& negotiated) {
    json b = d.body;
    if (negotiated) {
        b["supportedFeatures"] = *negotiated;
    } else {
        b.erase("supportedFeatures");
    }
    return b;
}

} // namespace

void add_dr_routes(sbi_core::http2::Server& server, Ctx& ctx) {
    const std::string root = kDrRoot;
    const std::string api_uri = root; // realm attribute of WWW-Authenticate
    const std::string scope = "nudsf-dr";
    const std::string recs = root + "/{realmId}/{storageId}/records";
    const std::string rec = recs + "/{recordId}";

    // Common prologue: authorize + resolve realm/storage.
    auto pre = [&ctx, api_uri, scope](const Request& req) -> tl::expected<StorageRef, Response> {
        if (auto deny = authorize(ctx, req, scope, api_uri)) {
            return tl::unexpected(*deny);
        }
        return resolve_storage(ctx, req);
    };

    // ---- GetRecord (5.2.2.2.2) ---------------------------------------------------------------
    server.add_route("GET", rec, instrument("GetRecord", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        auto r = ctx.store.get_record(*s, id);
        if (!r) {
            return not_found("record", id, "RECORD_NOT_FOUND");
        }
        const auto c = conditions(req);
        if (c.if_match && !if_match_passes(*c.if_match, r->etag)) {
            auto resp = extended_problem(412, "Precondition Failed", "If-Match did not match",
                                         "INCORRECT_CONDITIONAL_GET_REQUEST", r->meta);
            resp.headers.emplace("etag", r->etag);
            return resp;
        }
        if (c.not_modified(r->etag, r->lm)) {
            return not_modified(ctx, r->etag, r->lm);
        }
        return record_response(ctx, 200, *r, true);
    }));

    // ---- CreateOrModifyRecord (5.2.2.3.2 create, 5.2.2.4.2 update) -----------------------------
    server.add_route("PUT", rec, instrument("CreateOrModifyRecord", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        auto parsed = parse_record_body(req);
        if (!parsed) {
            return parsed.error();
        }
        const bool get_previous = query_flag(req, "get-previous");
        const bool clamped = clamp_ttl(ctx, parsed->meta);
        const auto c = conditions(req);
        std::optional<Response> early;
        auto tx = ctx.store.modify_record(
            *s, id, [&](const std::optional<Record>& before, Record& next) {
                early.reset();
                if (!c.write_passes(before ? std::optional<std::string>(before->etag)
                                           : std::nullopt)) {
                    Response r = get_previous && before ? record_response(ctx, 412, *before, false)
                                                        : empty(412);
                    if (before) {
                        r.headers.emplace("etag", before->etag);
                    }
                    early = r;
                    return Verdict::Abort;
                }
                if (before && clamped && get_previous) {
                    early = problem(403, "Forbidden",
                                    "the ttl exceeds the maximum this UDSF allows",
                                    std::string("TTL_VALUE_NOT_ALLOWED"));
                    return Verdict::Abort;
                }
                // Existing meta and blocks are discarded and replaced (6.1.3.3.3.2).
                next.meta = parsed->meta;
                next.blocks = parsed->blocks;
                for (auto& b : next.blocks) {
                    b.etag.clear();
                }
                return Verdict::Write;
            });
        if (early) {
            return *early;
        }
        if (!tx.before) {
            auto r = record_response(ctx, 201, tx.after, false);
            r.headers.emplace("location", dr_uri(ctx, *s, "records/" + id));
            return r;
        }
        if (get_previous) {
            auto r = record_response(ctx, 200, *tx.before, false);
            r.headers.erase("etag");
            r.headers.erase("last-modified");
            add_validators(ctx, r, tx.after.etag, tx.after.lm, false);
            return r;
        }
        if (clamped) {
            return record_response(ctx, 200, tx.after, false); // 5.2.2.4.2 2b: ttl policy
        }
        Response r = empty(204);
        add_validators(ctx, r, tx.after.etag, tx.after.lm, true);
        return r;
    }));

    // ---- PatchRecord (5.2.2.4.8, PartialRecordUpdate) ----------------------------------------------
    server.add_route("PATCH", rec, instrument("PatchRecord", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        const auto ct = header(req, "content-type").value_or("");
        if (!mp::is_multipart(ct)) {
            auto r = problem(415, "Unsupported Media Type",
                             "a RecordPatch is sent as multipart/mixed (TS 29.598 6.1.2.4.5)");
            r.headers.emplace("accept-patch", "multipart/mixed");
            return r;
        }
        auto parts = mp::parse_any(ct, req.body);
        if (!parts) {
            return problem(400, "Malformed multipart body", parts.error(),
                           std::string("INVALID_MSG_FORMAT"));
        }
        auto items = parse_patch_items((*parts)[0].body);
        if (!items) {
            return problem(400, "Bad Request", items.error(), std::string("INVALID_MSG_FORMAT"));
        }
        // Block values are Content-Ids of the following parts (6.1.2.4.5). They are swapped for
        // internal handles before applying so that JSON Patch moves/copies handles, not bytes.
        std::map<std::string, const mp::Part*> by_cid;
        for (std::size_t i = 1; i < parts->size(); ++i) {
            if ((*parts)[i].content_id) {
                by_cid[*(*parts)[i].content_id] = &(*parts)[i];
            }
        }
        const std::string kOld = std::string(1, '\0') + "old:";
        const std::string kNew = std::string(1, '\0') + "new:";
        sbi_gen::PatchResult bad;
        for (auto& it : *items) {
            const bool meta_path = it.path == "/meta" || it.path.rfind("/meta/", 0) == 0;
            const bool block_path = it.path.rfind("/blocks/", 0) == 0 && it.path.size() > 8 &&
                                    it.path.find('/', 8) == std::string::npos;
            if (!meta_path && !block_path) {
                bad.report.push_back({it.path, std::string("paths start with /meta or are "
                                                           "/blocks/{blockId} (6.1.2.4.5)")});
                continue;
            }
            if (block_path && it.value) {
                if (!it.value->is_string() || by_cid.count(it.value->get<std::string>()) == 0) {
                    bad.report.push_back(
                        {it.path, std::string("a block value names the Content-Id of a part")});
                    continue;
                }
                it.value = kNew + it.value->get<std::string>();
            }
        }
        if (!bad.report.empty()) {
            return json_response(422, json(bad));
        }
        const auto c = conditions(req);
        std::optional<Response> early;
        auto tx = ctx.store.modify_record(
            *s, id, [&](const std::optional<Record>& before, Record& next) {
                early.reset();
                if (!before) {
                    early = not_found("record", id, "RECORD_NOT_FOUND");
                    return Verdict::Abort;
                }
                if (!c.write_passes(before->etag)) {
                    auto r = extended_problem(412, "Precondition Failed",
                                              "a request precondition evaluated to false",
                                              "INCORRECT_CONDITIONAL_GET_REQUEST", before->meta);
                    r.headers.emplace("etag", before->etag);
                    early = r;
                    return Verdict::Abort;
                }
                json doc{{"meta", before->meta}, {"blocks", json::object()}};
                for (const auto& b : before->blocks) {
                    doc["blocks"][b.id] = kOld + b.id;
                }
                auto patched = apply_atomic(doc, *items);
                auto fail = [&](const std::string& path, const std::string& why) {
                    sbi_gen::PatchResult pr;
                    pr.report.push_back({path, why});
                    early = json_response(422, json(pr));
                    return Verdict::Abort;
                };
                if (!patched) {
                    early = json_response(422, json(patched.error()));
                    return Verdict::Abort;
                }
                if (!patched->is_object() || patched->size() != 2 || !patched->contains("meta") ||
                    !(*patched)["blocks"].is_object()) {
                    return fail("/", "the patch shall leave a record of meta and blocks");
                }
                if (auto why = validate_record_meta((*patched)["meta"]); !why.empty()) {
                    return fail("/meta", why);
                }
                json meta = (*patched)["meta"];
                clamp_ttl(ctx, meta);
                next.meta = meta;
                next.blocks.clear();
                for (auto b = (*patched)["blocks"].begin(); b != (*patched)["blocks"].end(); ++b) {
                    const std::string h = b->is_string() ? b->get<std::string>() : "";
                    Block nb;
                    if (h.rfind(kOld, 0) == 0) {
                        const Block* old = before->find_block(h.substr(kOld.size()));
                        if (old == nullptr) {
                            return fail("/blocks/" + b.key(), "unknown block");
                        }
                        nb = *old;
                        if (old->id != b.key()) {
                            nb.etag.clear(); // copied/moved to a new id: a new block
                        }
                    } else if (h.rfind(kNew, 0) == 0) {
                        const auto* p = by_cid.at(h.substr(kNew.size()));
                        nb.content_type =
                            p->content_type.empty() ? "application/octet-stream" : p->content_type;
                        nb.cte = p->content_transfer_encoding.value_or("binary");
                        nb.data = p->body;
                    } else {
                        return fail("/blocks/" + b.key(),
                                    "a block value names the Content-Id of a part");
                    }
                    nb.id = b.key();
                    next.blocks.push_back(std::move(nb));
                }
                return Verdict::Write;
            });
        if (early) {
            return *early;
        }
        Response r = empty(204);
        add_validators(ctx, r, tx.after.etag, tx.after.lm, true);
        return r;
    }));

    // ---- DeleteRecord (5.2.2.5.2) --------------------------------------------------------------
    server.add_route("DELETE", rec, instrument("DeleteRecord", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        const bool get_previous = query_flag(req, "get-previous");
        const auto c = conditions(req);
        std::optional<Response> early;
        auto tx = ctx.store.modify_record(
            *s, id, [&](const std::optional<Record>& before, Record&) {
                early.reset();
                if (!before) {
                    early = not_found("record", id, "RECORD_NOT_FOUND");
                    return Verdict::Abort;
                }
                if (!c.write_passes(before->etag)) {
                    Response r = get_previous ? record_response(ctx, 412, *before, false)
                                              : empty(412);
                    r.headers.erase("etag");
                    r.headers.emplace("etag", before->etag);
                    early = r;
                    return Verdict::Abort;
                }
                return Verdict::Delete;
            });
        if (early) {
            return *early;
        }
        if (get_previous) {
            return record_response(ctx, 200, *tx.before, false);
        }
        return empty(204);
    }));

    // ---- GetMeta (5.2.2.2.3) -------------------------------------------------------------------
    server.add_route("GET", rec + "/meta", instrument("GetMeta", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        auto r = ctx.store.get_record(*s, id);
        if (!r) {
            return not_found("record", id, "RECORD_NOT_FOUND");
        }
        const auto c = conditions(req);
        if (c.if_match && !if_match_passes(*c.if_match, r->etag)) {
            auto resp = extended_problem(412, "Precondition Failed", "If-Match did not match",
                                         "INCORRECT_CONDITIONAL_GET_REQUEST", r->meta);
            resp.headers.emplace("etag", r->etag);
            return resp;
        }
        if (c.not_modified(r->etag, r->lm)) {
            return not_modified(ctx, r->etag, r->lm);
        }
        Response resp = json_response(200, r->meta);
        add_validators(ctx, resp, r->etag, r->lm, true);
        return resp;
    }));

    // ---- UpdateMeta (5.2.2.4.4) ------------------------------------------------------------------
    server.add_route("PATCH", rec + "/meta", instrument("UpdateMeta", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        auto items = parse_patch_items(req.body);
        if (!items) {
            return problem(400, "Bad Request", items.error(), std::string("INVALID_MSG_FORMAT"));
        }
        const auto c = conditions(req);
        std::optional<Response> early;
        std::vector<sbi_gen::ReportItem> discarded;
        auto tx = ctx.store.modify_record(
            *s, id, [&](const std::optional<Record>& before, Record& next) {
                early.reset();
                discarded.clear();
                if (!before) {
                    early = not_found("record", id, "RECORD_NOT_FOUND");
                    return Verdict::Abort;
                }
                if (!c.write_passes(before->etag)) {
                    auto r = extended_problem(412, "Precondition Failed",
                                              "a request precondition evaluated to false",
                                              "INCORRECT_CONDITIONAL_GET_REQUEST", before->meta);
                    r.headers.emplace("etag", before->etag);
                    early = r;
                    return Verdict::Abort;
                }
                auto res = apply_itemwise(before->meta, *items, [&](const json& m) {
                    auto why = validate_record_meta(m);
                    if (why.empty() && ctx.settings.max_record_ttl_seconds > 0 && m.contains("ttl") &&
                        to_ms(m["ttl"].get<std::string>()) >
                            now_ms() + ctx.settings.max_record_ttl_seconds * 1000) {
                        why = "TTL_VALUE_NOT_ALLOWED: the ttl exceeds the maximum this UDSF allows";
                    }
                    return why;
                });
                discarded = res.discarded;
                if (res.discarded.size() == items->size()) {
                    return Verdict::Abort; // nothing applied -- no write, report only
                }
                next.meta = res.document;
                return Verdict::Write;
            });
        if (early) {
            return *early;
        }
        const std::string etag = tx.committed ? tx.after.etag : tx.before->etag;
        const std::int64_t lm = tx.committed ? tx.after.lm : tx.before->lm;
        if (!discarded.empty()) {
            sbi_gen::PatchResult pr;
            pr.report = discarded;
            Response r = json_response(200, json(pr));
            add_validators(ctx, r, etag, lm, false);
            return r;
        }
        Response r = empty(204);
        add_validators(ctx, r, etag, lm, true);
        return r;
    }));

    // ---- GetBlockList (5.2.2.2.4) ------------------------------------------------------------
    server.add_route("GET", rec + "/blocks", instrument("GetBlockList", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        auto r = ctx.store.get_record(*s, id);
        if (!r) {
            return not_found("record", id, "RECORD_NOT_FOUND");
        }
        if (r->blocks.empty()) {
            return empty(204);
        }
        std::vector<mp::Part> parts;
        for (const auto& b : r->blocks) {
            mp::Part p{b.content_type, b.id, b.data};
            p.content_transfer_encoding = b.cte;
            parts.push_back(std::move(p));
        }
        const auto enc = mp::encode_subtype("parallel", parts); // 6.1.2.4.3
        Response resp;
        resp.status = 200;
        resp.headers.emplace("content-type", enc.content_type_header);
        resp.body = enc.body;
        add_validators(ctx, resp, r->etag, r->lm, true);
        return resp;
    }));

    // ---- GetBlock (5.2.2.2.5) -----------------------------------------------------------------
    const std::string blk = rec + "/blocks/{blockId}";
    server.add_route("GET", blk, instrument("GetBlock", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        const auto bid = req.path_params.at("blockId");
        auto r = ctx.store.get_record(*s, id);
        if (!r) {
            return not_found("record", id, "RECORD_NOT_FOUND");
        }
        const Block* b = r->find_block(bid);
        if (b == nullptr) {
            return not_found("block", bid, "BLOCK_NOT_FOUND");
        }
        const auto c = conditions(req);
        if (c.if_match && !if_match_passes(*c.if_match, b->etag)) {
            auto resp = extended_problem(412, "Precondition Failed", "If-Match did not match",
                                         "INCORRECT_CONDITIONAL_GET_REQUEST", r->meta);
            resp.headers.emplace("etag", b->etag);
            return resp;
        }
        if (c.not_modified(b->etag, b->lm)) {
            return not_modified(ctx, b->etag, b->lm);
        }
        return block_response(ctx, 200, *b, true);
    }));

    // ---- CreateOrModifyBlock (5.2.2.3.3 create, 5.2.2.4.3 update) -------------------------------
    server.add_route("PUT", blk, instrument("CreateOrModifyBlock", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        const auto bid = req.path_params.at("blockId");
        const bool get_previous = query_flag(req, "get-previous");
        const auto c = conditions(req);
        Block nb;
        nb.id = bid;
        nb.content_type = header(req, "content-type").value_or("application/octet-stream");
        nb.cte = header(req, "content-transfer-encoding").value_or("binary");
        nb.data = req.body;
        std::optional<Response> early;
        std::optional<Block> previous;
        auto tx = ctx.store.modify_record(
            *s, id, [&](const std::optional<Record>& before, Record& next) {
                early.reset();
                previous.reset();
                if (!before) {
                    early = not_found("record", id, "RECORD_NOT_FOUND");
                    return Verdict::Abort;
                }
                const Block* old = before->find_block(bid);
                if (old != nullptr) {
                    previous = *old;
                }
                if (!c.write_passes(old ? std::optional<std::string>(old->etag) : std::nullopt)) {
                    Response r = get_previous && old ? block_response(ctx, 412, *old, false)
                                                     : empty(412);
                    r.headers.erase("etag");
                    if (old != nullptr) {
                        r.headers.emplace("etag", old->etag);
                    }
                    early = r;
                    return Verdict::Abort;
                }
                next.blocks.erase(std::remove_if(next.blocks.begin(), next.blocks.end(),
                                                 [&](const Block& b) { return b.id == bid; }),
                                  next.blocks.end());
                next.blocks.push_back(nb); // etag empty -> fresh validator
                return Verdict::Write;
            });
        if (early) {
            return *early;
        }
        const Block* stored = tx.after.find_block(bid);
        if (!previous) {
            Response r = empty(201);
            r.headers.emplace("location", dr_uri(ctx, *s, "records/" + id + "/blocks/" + bid));
            add_validators(ctx, r, stored->etag, stored->lm, false);
            return r;
        }
        if (get_previous) {
            Response r = block_response(ctx, 200, *previous, false);
            r.headers.erase("etag");
            r.headers.erase("last-modified");
            add_validators(ctx, r, stored->etag, stored->lm, false);
            return r;
        }
        Response r = empty(204);
        add_validators(ctx, r, stored->etag, stored->lm, true);
        return r;
    }));

    // ---- DeleteBlock (5.2.2.5.3) ----------------------------------------------------------------
    server.add_route("DELETE", blk, instrument("DeleteBlock", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("recordId");
        const auto bid = req.path_params.at("blockId");
        const bool get_previous = query_flag(req, "get-previous");
        const auto c = conditions(req);
        std::optional<Response> early;
        std::optional<Block> previous;
        ctx.store.modify_record(*s, id, [&](const std::optional<Record>& before, Record& next) {
            early.reset();
            if (!before) {
                early = not_found("record", id, "RECORD_NOT_FOUND");
                return Verdict::Abort;
            }
            const Block* old = before->find_block(bid);
            if (old == nullptr) {
                early = not_found("block", bid, "BLOCK_NOT_FOUND");
                return Verdict::Abort;
            }
            previous = *old;
            if (!c.write_passes(old->etag)) {
                Response r = get_previous ? block_response(ctx, 412, *old, false) : empty(412);
                r.headers.erase("etag");
                r.headers.emplace("etag", old->etag);
                early = r;
                return Verdict::Abort;
            }
            next.blocks.erase(std::remove_if(next.blocks.begin(), next.blocks.end(),
                                             [&](const Block& b) { return b.id == bid; }),
                              next.blocks.end());
            return Verdict::Write;
        });
        if (early) {
            return *early;
        }
        if (get_previous) {
            return block_response(ctx, 200, *previous, false);
        }
        return empty(204);
    }));

    // ---- SearchRecord (5.2.2.2.6) ----------------------------------------------------------------
    server.add_route("GET", recs, instrument("SearchRecord", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        if (query(req, "tag-count-filter")) {
            return problem(400, "Bad Request",
                           "tag-count-filter needs the AdvancedCounting feature, which this UDSF "
                           "does not support (ADR-0400)",
                           std::string("INVALID_QUERY_PARAM"));
        }
        const auto filter_text = query(req, "filter");
        if (!filter_text) {
            return problem(400, "Bad Request", "the filter query parameter is required",
                           std::string("MANDATORY_QUERY_PARAM_MISSING"));
        }
        auto expr = parse_filter_param(*filter_text);
        if (!expr) {
            return problem(400, "Bad Request", expr.error(), std::string("INVALID_QUERY_PARAM"));
        }
        const bool count_only = query_flag(req, "count-indicator");
        const auto retrieve = query(req, "retrieve-records");
        if (retrieve && *retrieve != "ONLY_META" && *retrieve != "META_AND_BLOCKS") {
            return problem(400, "Bad Request", "unknown RetrieveRecords value " + *retrieve,
                           std::string("INVALID_QUERY_PARAM"));
        }
        if (count_only && retrieve) {
            return problem(400, "Bad Request",
                           "count-indicator and retrieve-records are mutually exclusive",
                           std::string("INVALID_QUERY_PARAM"));
        }
        auto limit = query_uint(req, "limit-range");
        auto max_kb = query_uint(req, "max-payload-size");
        if (!limit || !max_kb) {
            return problem(400, "Bad Request", !limit ? limit.error() : max_kb.error(),
                           std::string("INVALID_QUERY_PARAM"));
        }
        auto index = ctx.store.record_index(*s);
        auto matched = evaluate(*expr, *index);
        if (!matched) {
            return problem(400, "Bad Request", matched.error(), std::string("INVALID_QUERY_PARAM"));
        }
        if (matched->empty()) {
            return empty(204);
        }
        sbi_gen::RecordSearchResultDescriptor d;
        d.count = static_cast<std::int64_t>(matched->size());
        d.supportedFeatures = negotiate(kDrFeatures, query(req, "supported-features"));
        std::vector<std::string> ids(matched->begin(), matched->end());
        if (!count_only) {
            if (*limit && static_cast<std::size_t>(**limit) < ids.size()) {
                ids.resize(static_cast<std::size_t>(**limit));
            }
            std::vector<std::string> refs;
            for (const auto& id : ids) {
                refs.push_back(dr_uri(ctx, *s, "records/" + id));
            }
            d.references = refs;
        }
        if (!retrieve) {
            return json_response(200, json(d));
        }
        // CombinedSearchRetrieve (6.1.2.4.6): descriptor, then "<id>/meta" and "<id>/<blockId>".
        std::vector<mp::Part> parts;
        std::size_t size = 0;
        const std::size_t budget =
            *max_kb ? static_cast<std::size_t>(**max_kb) * 1024 : static_cast<std::size_t>(-1);
        for (const auto& id : ids) {
            auto r = ctx.store.get_record(*s, id);
            if (!r) {
                continue; // deleted between search and retrieval
            }
            std::vector<mp::Part> rp;
            rp.push_back({"application/json", id + "/meta", r->meta.dump()});
            if (*retrieve == "META_AND_BLOCKS") {
                for (const auto& b : r->blocks) {
                    mp::Part p{b.content_type, id + "/" + b.id, b.data};
                    p.content_transfer_encoding = b.cte;
                    rp.push_back(std::move(p));
                }
            }
            std::size_t rs = 0;
            for (const auto& p : rp) {
                rs += p.body.size();
            }
            if (size + rs > budget) {
                break; // max-payload-size: the consumer fetches the rest (BulkOperations)
            }
            size += rs;
            parts.insert(parts.end(), rp.begin(), rp.end());
        }
        parts.insert(parts.begin(),
                     mp::Part{"application/json", std::string("recordSearchResultDescriptor"),
                              json(d).dump()});
        const auto enc = mp::encode_subtype("mixed", parts);
        Response resp;
        resp.status = 200;
        resp.headers.emplace("content-type", enc.content_type_header);
        resp.body = enc.body;
        return resp;
    }));

    // ---- BulkDeleteRecords (5.2.2.5.5) ----------------------------------------------------------
    server.add_route("DELETE", recs, instrument("BulkDeleteRecords", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto filter_text = query(req, "filter");
        if (!filter_text) {
            return problem(400, "Bad Request", "the filter query parameter is required",
                           std::string("MANDATORY_QUERY_PARAM_MISSING"));
        }
        auto expr = parse_filter_param(*filter_text);
        if (!expr) {
            return problem(400, "Bad Request", expr.error(), std::string("INVALID_QUERY_PARAM"));
        }
        auto index = ctx.store.record_index(*s);
        auto matched = evaluate(*expr, *index);
        if (!matched) {
            return problem(400, "Bad Request", matched.error(), std::string("INVALID_QUERY_PARAM"));
        }
        std::vector<std::string> deleted;
        std::vector<std::string> failed;
        for (const auto& id : *matched) {
            try {
                auto tx = ctx.store.modify_record(*s, id, [](const std::optional<Record>& b, Record&) {
                    return b ? Verdict::Delete : Verdict::Abort;
                });
                if (tx.committed) {
                    deleted.push_back(id);
                }
            } catch (const std::exception& e) {
                spdlog::warn("udsf: bulk delete of {} failed: {}", id, e.what());
                failed.push_back(id);
            }
        }
        if (deleted.empty() && failed.empty()) {
            return empty(204);
        }
        const bool partial = feature_negotiated(query(req, "supported-features"), 7);
        if (!failed.empty() && !partial) {
            return problem(500, "Internal Server Error",
                           std::to_string(failed.size()) + " matching record(s) could not be "
                           "deleted; negotiate RecordDeletePartialSuccess for a partial report",
                           std::string("SYSTEM_FAILURE"));
        }
        if (deleted.empty()) {
            return problem(500, "Internal Server Error", "no matching record could be deleted",
                           std::string("SYSTEM_FAILURE"));
        }
        sbi_gen::RecordDeleteResponse out;
        out.recordIdList = deleted;
        if (!failed.empty()) {
            out.failedRecordIdList = failed;
        }
        return json_response(200, json(out));
    }));

    // ---- GetNotificationSubscriptions (5.2.2.2.7) -----------------------------------------------
    const std::string subs = root + "/{realmId}/{storageId}/subs-to-notify";
    server.add_route("GET", subs, instrument("GetNotificationSubscriptions", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        auto limit = query_uint(req, "limit-range");
        if (!limit) {
            return problem(400, "Bad Request", limit.error(), std::string("INVALID_QUERY_PARAM"));
        }
        const auto neg = negotiate(kDrFeatures, query(req, "supported-features"));
        json arr = json::array();
        for (const auto& d : ctx.store.list_subs(*s)) {
            if (*limit && static_cast<std::int64_t>(arr.size()) >= **limit) {
                break;
            }
            arr.push_back(sub_body(d, neg));
        }
        return json_response(200, arr);
    }));

    const std::string sub = subs + "/{subscriptionId}";
    // ---- GetNotificationSubscription (5.2.2.2.8) ------------------------------------------------
    server.add_route("GET", sub, instrument("GetNotificationSubscription", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("subscriptionId");
        auto d = ctx.store.get_sub(*s, id);
        if (!d) {
            return not_found("subscription", id, "SUBSCRIPTION_NOT_FOUND");
        }
        if (conditions(req).not_modified(d->etag, d->lm)) {
            return not_modified(ctx, d->etag, d->lm);
        }
        Response r = json_response(200, sub_body(*d, negotiate(kDrFeatures, query(req, "supported-features"))));
        add_validators(ctx, r, d->etag, d->lm, true);
        return r;
    }));

    // ---- CreateAndUpdateNotificationSubscription (5.2.2.7.2 subscribe, 5.2.2.4.6 update) --------
    server.add_route("PUT", sub, instrument("CreateAndUpdateNotificationSubscription", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("subscriptionId");
        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::parse_error& e) {
            return problem(400, "Malformed JSON", e.what(), std::string("INVALID_MSG_FORMAT"));
        }
        sbi_gen::NotificationSubscription ns;
        if (auto why = validate_subscription(body, &ns); !why.empty()) {
            return problem(400, "Bad Request", why, std::string("INVALID_MSG_FORMAT"));
        }
        // 5.2.2.7.2 2b: every monitored resource shall exist; 409 carries the missing ones.
        if (ns.subFilter && ns.subFilter->monitoredResourceUris) {
            json missing = json::array();
            for (const auto& u : *ns.subFilter->monitoredResourceUris) {
                const auto ref = parse_record_uri(u);
                if (!ref || ref->realm != s->realm || ref->storage != s->storage ||
                    !ctx.store.get_record(*s, ref->record_id)) {
                    missing.push_back(u);
                }
            }
            if (!missing.empty()) {
                return json_response(409, missing);
            }
        }
        apply_expiry_policy(ctx, ns);
        json stored = ns;
        stored.erase("supportedFeatures");
        const auto c = conditions(req);
        std::optional<Response> early;
        auto tx = ctx.store.modify_sub(
            *s, id,
            [&](const std::optional<Doc>& before, Doc& next) {
                early.reset();
                if (!c.write_passes(before ? std::optional<std::string>(before->etag)
                                           : std::nullopt)) {
                    Response r = empty(412);
                    if (before) {
                        r.headers.emplace("etag", before->etag);
                    }
                    early = r;
                    return Verdict::Abort;
                }
                if (before) {
                    const auto owner = before->body.at("clientId").get<sbi_gen::ClientId>();
                    if (client_key(owner) != client_key(ns.clientId)) {
                        early = problem(403, "Forbidden",
                                        "the subscription belongs to another client",
                                        std::string("SUBSCRIPTION_EXISTS"));
                        return Verdict::Abort;
                    }
                }
                next.body = stored;
                return Verdict::Write;
            },
            sub_due);
        if (early) {
            return *early;
        }
        const auto neg = negotiate(kDrFeatures, ns.supportedFeatures);
        if (!tx.before) {
            Response r = json_response(201, sub_body(tx.after, neg));
            r.headers.emplace("location", dr_uri(ctx, *s, "subs-to-notify/" + id));
            add_validators(ctx, r, tx.after.etag, tx.after.lm, false);
            return r;
        }
        Response r = json_response(200, sub_body(tx.after, neg));
        add_validators(ctx, r, tx.after.etag, tx.after.lm, false);
        return r;
    }));

    // ---- UpdateNotificationSubscription (5.2.2.4.5) ---------------------------------------------
    server.add_route("PATCH", sub, instrument("UpdateNotificationSubscription", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("subscriptionId");
        auto items = parse_patch_items(req.body);
        if (!items) {
            return problem(400, "Bad Request", items.error(), std::string("INVALID_MSG_FORMAT"));
        }
        const auto c = conditions(req);
        std::optional<Response> early;
        std::vector<sbi_gen::ReportItem> discarded;
        auto tx = ctx.store.modify_sub(
            *s, id,
            [&](const std::optional<Doc>& before, Doc& next) {
                early.reset();
                discarded.clear();
                if (!before) {
                    early = not_found("subscription", id, "SUBSCRIPTION_NOT_FOUND");
                    return Verdict::Abort;
                }
                if (!c.write_passes(before->etag)) {
                    Response r = empty(412);
                    r.headers.emplace("etag", before->etag);
                    early = r;
                    return Verdict::Abort;
                }
                const auto owner = before->body.at("clientId");
                auto res = apply_itemwise(before->body, *items, [&](const json& d) {
                    if (d.value("clientId", json()) != owner) {
                        return std::string("clientId identifies the owner and cannot change");
                    }
                    return validate_subscription(d, nullptr);
                });
                discarded = res.discarded;
                if (res.discarded.size() == items->size()) {
                    return Verdict::Abort;
                }
                auto ns = res.document.get<sbi_gen::NotificationSubscription>();
                apply_expiry_policy(ctx, ns);
                next.body = ns;
                next.body.erase("supportedFeatures");
                return Verdict::Write;
            },
            sub_due);
        if (early) {
            return *early;
        }
        const Doc& d = tx.committed ? tx.after : *tx.before;
        if (!discarded.empty()) {
            sbi_gen::PatchResult pr;
            pr.report = discarded;
            Response r = json_response(200, json(pr));
            add_validators(ctx, r, d.etag, d.lm, false);
            return r;
        }
        Response r = empty(204);
        add_validators(ctx, r, d.etag, d.lm, true);
        return r;
    }));

    // ---- DeleteNotificationSubscription (5.2.2.8.2) ---------------------------------------------
    server.add_route("DELETE", sub, instrument("DeleteNotificationSubscription", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("subscriptionId");
        // client-id is `schema: ClientId` with no `content:` -- OpenAPI's default for an object
        // query parameter is style=form, explode=true, so it arrives as ?nfId=..&/or nfSetId=..
        // (the name "client-id" itself is not on the wire). A JSON-encoded client-id=... is also
        // accepted (lenient; ADR-0401).
        sbi_gen::ClientId cid;
        if (auto j = query(req, "client-id")) {
            try {
                cid = json::parse(*j).get<sbi_gen::ClientId>();
            } catch (const std::exception& e) {
                return problem(400, "Bad Request", std::string("invalid client-id: ") + e.what(),
                               std::string("INVALID_QUERY_PARAM"));
            }
        } else {
            cid.nfId = query(req, "nfId");
            cid.nfSetId = query(req, "nfSetId");
        }
        if (!cid.nfId && !cid.nfSetId) {
            return problem(400, "Bad Request", "client-id (nfId and/or nfSetId) is required",
                           std::string("MANDATORY_QUERY_PARAM_MISSING"));
        }
        const bool get_previous = query_flag(req, "get-previous");
        const auto c = conditions(req);
        std::optional<Response> early;
        auto tx = ctx.store.modify_sub(
            *s, id,
            [&](const std::optional<Doc>& before, Doc&) {
                early.reset();
                if (!before) {
                    early = not_found("subscription", id, "SUBSCRIPTION_NOT_FOUND");
                    return Verdict::Abort;
                }
                if (client_key(before->body.at("clientId").get<sbi_gen::ClientId>()) !=
                    client_key(cid)) {
                    early = problem(403, "Forbidden", "client-id does not own this subscription",
                                    std::string("SUBSCRIPTION_EXISTS"));
                    return Verdict::Abort;
                }
                if (!c.write_passes(before->etag)) {
                    Response r = get_previous ? json_response(412, before->body) : empty(412);
                    r.headers.emplace("etag", before->etag);
                    early = r;
                    return Verdict::Abort;
                }
                return Verdict::Delete;
            },
            sub_due);
        if (early) {
            return *early;
        }
        if (get_previous) {
            return json_response(200, json::array({tx.before->body})); // YAML: array
        }
        return empty(204);
    }));

    // ---- Meta Schema: GetMetaSchema / CreateOrModifyMetaSchema / DeleteMetaSchema ---------------
    const std::string schema = root + "/{realmId}/{storageId}/meta-schemas/{schemaId}";
    server.add_route("GET", schema, instrument("GetMetaSchema", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("schemaId");
        auto d = ctx.store.get_schema(*s, id);
        if (!d) {
            return not_found("meta schema", id, "SCHEMA_NOT_FOUND");
        }
        if (conditions(req).not_modified(d->etag, d->lm)) {
            return not_modified(ctx, d->etag, d->lm);
        }
        // TS table 6.1.3.9.3.1-3: MetaSchema as JSON. The YAML's 200 references the multipart
        // Record response instead -- a YAML defect, see ADR-0401.
        Response r = json_response(200, d->body);
        add_validators(ctx, r, d->etag, d->lm, true);
        return r;
    }));

    server.add_route("PUT", schema, instrument("CreateOrModifyMetaSchema", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("schemaId");
        json body;
        sbi_gen::MetaSchema ms;
        try {
            body = json::parse(req.body);
            ms = body.get<sbi_gen::MetaSchema>();
        } catch (const std::exception& e) {
            return problem(400, "Bad Request", std::string("invalid MetaSchema: ") + e.what(),
                           std::string("INVALID_MSG_FORMAT"));
        }
        if (ms.schemaId != id) {
            return problem(400, "Bad Request", "schemaId in the body differs from the URI",
                           std::string("INVALID_MSG_FORMAT"));
        }
        for (const auto& t : ms.metaTags) {
            const auto& k = t.keyType.value;
            if ((k == "SEARCH_KEY" || k == "SEARCH_AND_COUNT_KEY") && !t.sort) {
                return problem(400, "Bad Request",
                               "TagType.sort shall be present for " + k + " (table 6.1.6.2.16-1)",
                               std::string("MANDATORY_IE_MISSING"));
            }
        }
        const bool get_previous = query_flag(req, "get-previous");
        const auto c = conditions(req);
        std::optional<Response> early;
        auto tx = ctx.store.modify_schema(*s, id, [&](const std::optional<Doc>& before, Doc& next) {
            early.reset();
            if (!c.write_passes(before ? std::optional<std::string>(before->etag) : std::nullopt)) {
                Response r = get_previous && before ? json_response(412, before->body) : empty(412);
                if (before) {
                    r.headers.emplace("etag", before->etag);
                }
                early = r;
                return Verdict::Abort;
            }
            next.body = json(ms);
            return Verdict::Write;
        });
        if (early) {
            return *early;
        }
        if (!tx.before) {
            Response r = json_response(201, tx.after.body);
            r.headers.emplace("location", dr_uri(ctx, *s, "meta-schemas/" + id));
            add_validators(ctx, r, tx.after.etag, tx.after.lm, false);
            return r;
        }
        Response r = get_previous ? json_response(200, tx.before->body) : empty(204);
        add_validators(ctx, r, tx.after.etag, tx.after.lm, false);
        return r;
    }));

    server.add_route("DELETE", schema, instrument("DeleteMetaSchema", [&ctx, pre](const Request& req) {
        auto s = pre(req);
        if (!s) {
            return s.error();
        }
        const auto id = req.path_params.at("schemaId");
        if (ctx.store.timers_using_schema(*s, id) > 0) {
            return problem(403, "Forbidden", "the meta schema is still referenced by timers",
                           std::string("SCHEMA_IN_USE"));
        }
        const bool get_previous = query_flag(req, "get-previous");
        const auto c = conditions(req);
        std::optional<Response> early;
        auto tx = ctx.store.modify_schema(*s, id, [&](const std::optional<Doc>& before, Doc&) {
            early.reset();
            if (!before) {
                early = not_found("meta schema", id, "SCHEMA_NOT_FOUND");
                return Verdict::Abort;
            }
            if (!c.write_passes(before->etag)) {
                Response r = get_previous ? json_response(412, before->body) : empty(412);
                r.headers.emplace("etag", before->etag);
                early = r;
                return Verdict::Abort;
            }
            return Verdict::Delete;
        });
        if (early) {
            return *early;
        }
        return get_previous ? json_response(200, tx.before->body) : empty(204);
    }));
}

} // namespace udsf
