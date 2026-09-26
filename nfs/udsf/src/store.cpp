#include "store.hpp"

#include "sbi_core/datetime.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

#include "TS29598_Nudsf_DataRepository.hpp"
#include "http_util.hpp"

namespace udsf {

namespace {

constexpr int kMaxAttempts = 32;

std::string tag_key(const std::string& p, const char* kind, const std::string& tag,
                    const std::string& value) {
    return p + kind + ":" + std::to_string(tag.size()) + ":" + tag + ":" + value;
}

std::string tagz_key(const std::string& p, const char* kind, const std::string& tag) {
    return p + kind + "z:" + std::to_string(tag.size()) + ":" + tag;
}

std::string zmember(const std::string& value, const std::string& id) {
    std::string m = value;
    m.push_back('\0');
    m += id;
    return m;
}

std::vector<std::string> run(sw::redis::Redis& r, std::vector<std::string> args) {
    std::vector<std::string> out;
    r.command(args.begin(), args.end(), std::back_inserter(out));
    return out;
}

std::int64_t ttl_ms_of(const nlohmann::json& meta) {
    if (!meta.is_object() || !meta.contains("ttl") || !meta["ttl"].is_string()) {
        return 0;
    }
    const auto tp = sbi_core::parse_rfc3339(meta["ttl"].get<std::string>());
    if (!tp) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp->time_since_epoch()).count();
}

template <typename Tx>
void queue_index(Tx& tx, const std::string& p, const char* kind, const std::string& id,
                 const nlohmann::json& tags, bool add) {
    for (const auto& [tag, value] : tag_pairs(tags)) {
        if (add) {
            tx.sadd(tag_key(p, kind, tag, value), id);
            tx.zadd(tagz_key(p, kind, tag), zmember(value, id), 0.0);
        } else {
            tx.srem(tag_key(p, kind, tag, value), id);
            tx.zrem(tagz_key(p, kind, tag), zmember(value, id));
        }
    }
}

class IndexImpl : public TagIndex {
public:
    IndexImpl(std::shared_ptr<sw::redis::Redis> redis, std::string prefix, const char* kind,
              std::string set_key, bool schema_capable)
        : redis_(std::move(redis)), p_(std::move(prefix)), kind_(kind),
          set_key_(std::move(set_key)), schema_capable_(schema_capable) {}

    std::set<std::string> all() override {
        std::set<std::string> out;
        redis_->smembers(p_ + set_key_, std::inserter(out, out.end()));
        return out;
    }
    std::set<std::string> eq(const std::string& tag, const std::string& value) override {
        std::set<std::string> out;
        redis_->smembers(tag_key(p_, kind_, tag, value), std::inserter(out, out.end()));
        return out;
    }
    std::set<std::string>
    range(const std::string& tag, const std::string& op, const std::string& value) override {
        // Members are "<value>\0<id>"; see store.hpp. Byte-wise lexicographic order is TS
        // 29.598 6.1.6.3.3's "lexicographical order".
        std::string lo = "-";
        std::string hi = "+";
        std::string v0 = value;
        v0.push_back('\0');
        std::string v1 = value;
        v1.push_back('\x01');
        if (op == "GT") {
            lo = "[" + v1;
        } else if (op == "GTE") {
            lo = "[" + v0;
        } else if (op == "LT") {
            hi = "(" + v0;
        } else { // LTE
            hi = "(" + v1;
        }
        std::set<std::string> out;
        for (const auto& m : run(*redis_, {"ZRANGEBYLEX", tagz_key(p_, kind_, tag), lo, hi})) {
            const auto nul = m.find('\0');
            if (nul != std::string::npos) {
                out.insert(m.substr(nul + 1));
            }
        }
        return out;
    }
    std::set<std::string> existing(const std::vector<std::string>& ids) override {
        std::set<std::string> out;
        for (const auto& id : ids) {
            if (redis_->sismember(p_ + set_key_, id)) {
                out.insert(id);
            }
        }
        return out;
    }
    std::optional<std::set<std::string>> with_schema(const std::string& schema_id) override {
        if (!schema_capable_) {
            return std::nullopt;
        }
        std::set<std::string> out;
        redis_->smembers(p_ + "tschema:" + schema_id, std::inserter(out, out.end()));
        return out;
    }

private:
    std::shared_ptr<sw::redis::Redis> redis_;
    std::string p_;
    const char* kind_;
    std::string set_key_;
    bool schema_capable_;
};

} // namespace

const Block* Record::find_block(const std::string& bid) const {
    for (const auto& b : blocks) {
        if (b.id == bid) {
            return &b;
        }
    }
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> tag_pairs(const nlohmann::json& tags) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!tags.is_object()) {
        return out;
    }
    for (auto it = tags.begin(); it != tags.end(); ++it) {
        if (!it->is_array()) {
            continue;
        }
        for (const auto& v : *it) {
            if (v.is_string()) {
                out.emplace_back(it.key(), v.get<std::string>());
            }
        }
    }
    return out;
}

std::optional<ResourceRef> parse_record_uri(const std::string& uri) {
    std::string path = uri;
    if (const auto scheme = path.find("://"); scheme != std::string::npos) {
        const auto slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? "" : path.substr(slash);
    }
    if (const auto q = path.find_first_of("?#"); q != std::string::npos) {
        path = path.substr(0, q);
    }
    std::vector<std::string> seg;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const auto slash = path.find('/', pos);
        std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos
                                                                        : slash - pos);
        if (!part.empty()) {
            seg.push_back(part);
        }
        if (slash == std::string::npos) {
            break;
        }
        pos = slash + 1;
    }
    if (seg.size() < 4 || seg[seg.size() - 2] != "records") {
        return std::nullopt;
    }
    return ResourceRef{seg[seg.size() - 4], seg[seg.size() - 3], seg.back()};
}

Store::Store(std::shared_ptr<sw::redis::Redis> redis, std::string dr_api_root)
    : redis_(std::move(redis)), dr_api_root_(std::move(dr_api_root)) {}

std::string Store::record_uri(const StorageRef& s, const std::string& id) const {
    return dr_api_root_ + "/" + s.realm + "/" + s.storage + "/records/" + id;
}

std::optional<Record>
Store::load_record(sw::redis::Redis& r, const StorageRef& s, const std::string& id) {
    const std::string p = s.prefix();
    std::unordered_map<std::string, std::string> h;
    r.hgetall(p + "rec:" + id, std::inserter(h, h.end()));
    if (h.empty() || h.count("meta") == 0) {
        return std::nullopt;
    }
    Record rec;
    rec.id = id;
    rec.meta = nlohmann::json::parse(h["meta"]);
    rec.etag = h["etag"];
    rec.lm = h.count("lm") != 0 ? std::stoll(h["lm"]) : 0;
    std::unordered_map<std::string, std::string> bm;
    std::unordered_map<std::string, std::string> bd;
    r.hgetall(p + "blkm:" + id, std::inserter(bm, bm.end()));
    r.hgetall(p + "blkd:" + id, std::inserter(bd, bd.end()));
    for (const auto& [bid, m] : bm) {
        const auto j = nlohmann::json::parse(m);
        Block b;
        b.id = bid;
        b.content_type = j.value("ct", "application/octet-stream");
        b.cte = j.value("cte", "binary");
        b.etag = j.value("etag", "");
        b.lm = j.value("lm", std::int64_t{0});
        b.data = bd[bid];
        rec.blocks.push_back(std::move(b));
    }
    std::sort(rec.blocks.begin(), rec.blocks.end(),
              [](const Block& a, const Block& b) { return a.id < b.id; });
    return rec;
}

std::optional<Record> Store::get_record(const StorageRef& s, const std::string& id) {
    return load_record(*redis_, s, id);
}

std::vector<Notification> Store::data_change_notifications(sw::redis::Redis& r,
                                                           const StorageRef& s,
                                                           const std::optional<Record>& before,
                                                           const std::optional<Record>& after) {
    std::vector<Notification> out;
    std::vector<std::string> ids;
    r.smembers(s.prefix() + "subs", std::back_inserter(ids));
    if (ids.empty()) {
        return out;
    }
    const std::string op = !before ? "CREATED" : (!after ? "DELETED" : "UPDATED");
    const Record& rec = after ? *after : *before;
    const auto now = now_ms();
    for (const auto& sid : ids) {
        const auto body = r.hget(s.prefix() + "sub:" + sid, "body");
        if (!body) {
            continue;
        }
        sbi_gen::NotificationSubscription sub;
        try {
            sub = nlohmann::json::parse(*body).get<sbi_gen::NotificationSubscription>();
        } catch (const std::exception&) {
            continue;
        }
        if (sub.expiry) {
            if (const auto tp = sbi_core::parse_rfc3339(*sub.expiry)) {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(tp->time_since_epoch())
                        .count() <= now) {
                    continue; // expired, awaiting the sweeper
                }
            }
        }
        bool monitored = false;
        if (sub.subFilter && sub.subFilter->monitoredResourceUris) {
            monitored = true;
            bool hit = false;
            for (const auto& u : *sub.subFilter->monitoredResourceUris) {
                const auto ref = parse_record_uri(u);
                if (ref && ref->realm == s.realm && ref->storage == s.storage &&
                    ref->record_id == rec.id) {
                    hit = true;
                }
            }
            if (!hit) {
                continue;
            }
        }
        if (monitored && op == "CREATED") {
            continue; // 6.1.6.2.13: with monitoredResourceUris only UPDATED/DELETED apply
        }
        if (sub.subFilter && sub.subFilter->operations && !sub.subFilter->operations->empty()) {
            const auto& ops = *sub.subFilter->operations;
            if (std::none_of(ops.begin(), ops.end(), [&](const auto& o) { return o.value == op; })) {
                continue;
            }
        }
        sbi_gen::NotificationDescription d;
        d.recordRef = record_uri(s, rec.id);
        d.operationType.value = op;
        d.subscriptionId = sid;
        nlohmann::json blocks = nlohmann::json::array();
        for (const auto& b : rec.blocks) {
            blocks.push_back({{"id", b.id},
                              {"ct", b.content_type},
                              {"cte", b.cte},
                              {"b64", base64_encode(b.data)}});
        }
        out.push_back({{"kind", "onDataChange"},
                       {"url", sub.callbackReference},
                       {"descriptor", nlohmann::json(d)},
                       {"meta", rec.meta},
                       {"blocks", std::move(blocks)}});
    }
    return out;
}

Store::RecordTx
Store::modify_record(const StorageRef& s,
                     const std::string& id,
                     const std::function<Verdict(const std::optional<Record>&, Record&)>& fn) {
    const std::string p = s.prefix();
    const std::string rec_key = p + "rec:" + id;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto tx = redis_->transaction(false, false);
        auto r = tx.redis();
        r.watch(rec_key);
        RecordTx out;
        out.before = load_record(r, s, id);
        out.after = out.before ? *out.before : Record{};
        out.after.id = id;
        const Verdict v = fn(out.before, out.after);
        if (v == Verdict::Abort) {
            r.unwatch();
            return out;
        }
        const auto now_s = now_ms() / 1000;
        if (v == Verdict::Write) {
            out.after.etag = new_etag();
            out.after.lm = now_s;
            for (auto& b : out.after.blocks) {
                if (b.etag.empty()) {
                    b.etag = new_etag();
                    b.lm = now_s;
                }
            }
        }
        const auto notes = data_change_notifications(
            r, s, out.before,
            v == Verdict::Delete ? std::nullopt : std::optional<Record>(out.after));

        if (out.before) {
            queue_index(tx, p, "rtag", id, out.before->meta.value("tags", nlohmann::json()), false);
        }
        if (v == Verdict::Delete) {
            tx.del(rec_key);
            tx.del(p + "blkd:" + id);
            tx.del(p + "blkm:" + id);
            tx.srem(p + "records", id);
            tx.zrem(p + "due:rec", id);
        } else {
            tx.hset(rec_key, "meta", out.after.meta.dump());
            tx.hset(rec_key, "etag", out.after.etag);
            tx.hset(rec_key, "lm", std::to_string(out.after.lm));
            tx.sadd(p + "records", id);
            queue_index(tx, p, "rtag", id, out.after.meta.value("tags", nlohmann::json()), true);
            if (out.before) {
                for (const auto& old : out.before->blocks) {
                    if (out.after.find_block(old.id) == nullptr) {
                        tx.hdel(p + "blkd:" + id, old.id);
                        tx.hdel(p + "blkm:" + id, old.id);
                    }
                }
            }
            for (const auto& b : out.after.blocks) {
                const Block* old = out.before ? out.before->find_block(b.id) : nullptr;
                if (old != nullptr && old->etag == b.etag) {
                    continue;
                }
                tx.hset(p + "blkd:" + id, b.id, b.data);
                tx.hset(p + "blkm:" + id,
                        b.id,
                        nlohmann::json{{"ct", b.content_type},
                                       {"cte", b.cte},
                                       {"etag", b.etag},
                                       {"lm", b.lm}}
                            .dump());
            }
            if (const auto ttl = ttl_ms_of(out.after.meta); ttl > 0) {
                tx.zadd(p + "due:rec", id, static_cast<double>(ttl));
            } else {
                tx.zrem(p + "due:rec", id);
            }
        }
        for (const auto& n : notes) {
            tx.lpush(p + "notifq", n.dump());
        }
        try {
            tx.exec();
            out.committed = true;
            return out;
        } catch (const sw::redis::WatchError&) {
            continue;
        }
    }
    throw std::runtime_error("udsf: record " + id + " kept changing under a write (contention)");
}

std::unique_ptr<TagIndex> Store::record_index(const StorageRef& s) {
    return std::make_unique<IndexImpl>(redis_, s.prefix(), "rtag", "records", false);
}

std::unique_ptr<TagIndex> Store::timer_index(const StorageRef& s) {
    return std::make_unique<IndexImpl>(redis_, s.prefix(), "ttag", "timers", true);
}

// ---- generic documents -------------------------------------------------------------------------

std::optional<Doc> Store::load_doc(sw::redis::Redis& r, const std::string& key, const std::string& id) {
    std::unordered_map<std::string, std::string> h;
    r.hgetall(key, std::inserter(h, h.end()));
    if (h.empty() || h.count("body") == 0) {
        return std::nullopt;
    }
    Doc d;
    d.id = id;
    d.body = nlohmann::json::parse(h["body"]);
    d.etag = h["etag"];
    d.lm = h.count("lm") != 0 ? std::stoll(h["lm"]) : 0;
    d.next_ms = h.count("next") != 0 ? std::stoll(h["next"]) : 0;
    d.left = h.count("left") != 0 ? std::stoll(h["left"]) : 0;
    return d;
}

namespace {

// One optimistic transaction over a Doc hash; `extra(tx, before, after-or-null)` queues the
// type-specific index/schedule writes.
template <typename Extra>
Store::DocTx modify_doc(sw::redis::Redis& redis,
                        const std::string& key,
                        const std::string& set_key,
                        const std::string& id,
                        const std::function<std::optional<Doc>(sw::redis::Redis&)>& load,
                        const std::function<Verdict(const std::optional<Doc>&, Doc&)>& fn,
                        Extra extra) {
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto tx = redis.transaction(false, false);
        auto r = tx.redis();
        r.watch(key);
        Store::DocTx out;
        out.before = load(r);
        out.after = out.before ? *out.before : Doc{};
        out.after.id = id;
        const Verdict v = fn(out.before, out.after);
        if (v == Verdict::Abort) {
            r.unwatch();
            return out;
        }
        if (v == Verdict::Delete) {
            tx.del(key);
            tx.srem(set_key, id);
            extra(tx, out.before, static_cast<const Doc*>(nullptr));
        } else {
            out.after.etag = new_etag();
            out.after.lm = now_ms() / 1000;
            tx.hset(key, "body", out.after.body.dump());
            tx.hset(key, "etag", out.after.etag);
            tx.hset(key, "lm", std::to_string(out.after.lm));
            tx.hset(key, "next", std::to_string(out.after.next_ms));
            tx.hset(key, "left", std::to_string(out.after.left));
            tx.sadd(set_key, id);
            extra(tx, out.before, &out.after);
        }
        try {
            tx.exec();
            out.committed = true;
            return out;
        } catch (const sw::redis::WatchError&) {
            continue;
        }
    }
    throw std::runtime_error("udsf: resource " + id + " kept changing under a write (contention)");
}

} // namespace

std::optional<Doc> Store::get_sub(const StorageRef& s, const std::string& id) {
    return load_doc(*redis_, s.prefix() + "sub:" + id, id);
}

std::vector<Doc> Store::list_subs(const StorageRef& s) {
    std::vector<std::string> ids;
    redis_->smembers(s.prefix() + "subs", std::back_inserter(ids));
    std::sort(ids.begin(), ids.end());
    std::vector<Doc> out;
    for (const auto& id : ids) {
        if (auto d = get_sub(s, id)) {
            out.push_back(std::move(*d));
        }
    }
    return out;
}

Store::DocTx
Store::modify_sub(const StorageRef& s,
                  const std::string& id,
                  const std::function<Verdict(const std::optional<Doc>&, Doc&)>& fn,
                  const std::function<std::pair<std::int64_t, std::int64_t>(const Doc&)>& due) {
    const std::string p = s.prefix();
    const std::string key = p + "sub:" + id;
    return modify_doc(
        *redis_, key, p + "subs", id,
        [&](sw::redis::Redis& r) { return load_doc(r, key, id); }, fn,
        [&](auto& tx, const std::optional<Doc>&, const Doc* after) {
            tx.zrem(p + "due:subexp", id);
            tx.zrem(p + "due:subnot", id);
            if (after != nullptr) {
                const auto [exp, notify] = due(*after);
                if (exp > 0) {
                    tx.zadd(p + "due:subexp", id, static_cast<double>(exp));
                }
                if (notify > 0) {
                    tx.zadd(p + "due:subnot", id, static_cast<double>(notify));
                }
            }
        });
}

std::optional<Doc> Store::get_schema(const StorageRef& s, const std::string& id) {
    return load_doc(*redis_, s.prefix() + "schema:" + id, id);
}

Store::DocTx
Store::modify_schema(const StorageRef& s,
                     const std::string& id,
                     const std::function<Verdict(const std::optional<Doc>&, Doc&)>& fn) {
    const std::string p = s.prefix();
    const std::string key = p + "schema:" + id;
    return modify_doc(
        *redis_, key, p + "schemas", id,
        [&](sw::redis::Redis& r) { return load_doc(r, key, id); }, fn,
        [](auto&, const std::optional<Doc>&, const Doc*) {});
}

std::int64_t Store::timers_using_schema(const StorageRef& s, const std::string& schema_id) {
    return redis_->scard(s.prefix() + "tschema:" + schema_id);
}

std::optional<Doc> Store::get_timer(const StorageRef& s, const std::string& id) {
    return load_doc(*redis_, s.prefix() + "tmr:" + id, id);
}

Store::DocTx Store::modify_timer(const StorageRef& s,
                                 const std::string& id,
                                 const std::function<Verdict(const std::optional<Doc>&, Doc&)>& fn,
                                 const std::function<std::int64_t(const Doc&)>& del) {
    const std::string p = s.prefix();
    const std::string key = p + "tmr:" + id;
    return modify_doc(
        *redis_, key, p + "timers", id,
        [&](sw::redis::Redis& r) { return load_doc(r, key, id); }, fn,
        [&](auto& tx, const std::optional<Doc>& before, const Doc* after) {
            if (before) {
                queue_index(tx, p, "ttag", id, before->body.value("metaTags", nlohmann::json()),
                            false);
                if (before->body.contains("schemaId") && before->body["schemaId"].is_string()) {
                    tx.srem(p + "tschema:" + before->body["schemaId"].get<std::string>(), id);
                }
            }
            tx.zrem(p + "due:tmr", id);
            tx.zrem(p + "due:tmrdel", id);
            if (after != nullptr) {
                queue_index(tx, p, "ttag", id, after->body.value("metaTags", nlohmann::json()),
                            true);
                if (after->body.contains("schemaId") && after->body["schemaId"].is_string()) {
                    tx.sadd(p + "tschema:" + after->body["schemaId"].get<std::string>(), id);
                }
                if (after->next_ms > 0) {
                    tx.zadd(p + "due:tmr", id, static_cast<double>(after->next_ms));
                }
                if (const auto at = del(*after); at > 0) {
                    tx.zadd(p + "due:tmrdel", id, static_cast<double>(at));
                }
            }
        });
}

std::vector<std::string> Store::claim_due(const StorageRef& s, const std::string& which,
                                          std::int64_t now, long limit) {
    const std::string key = s.prefix() + "due:" + which;
    std::vector<std::string> claimed;
    for (const auto& m : run(*redis_, {"ZRANGEBYSCORE", key, "-inf", std::to_string(now), "LIMIT",
                                       "0", std::to_string(limit)})) {
        if (redis_->zrem(key, m) == 1) {
            claimed.push_back(m);
        }
    }
    return claimed;
}

void Store::schedule(const StorageRef& s, const std::string& which, const std::string& member,
                     std::int64_t at_ms) {
    redis_->zadd(s.prefix() + "due:" + which, member, static_cast<double>(at_ms));
}

void Store::enqueue(const StorageRef& s, const Notification& n) {
    redis_->lpush(s.prefix() + "notifq", n.dump());
}

std::vector<Notification> Store::dequeue(const StorageRef& s, long max) {
    std::vector<Notification> out;
    for (long i = 0; i < max; ++i) {
        auto v = redis_->rpop(s.prefix() + "notifq");
        if (!v) {
            break;
        }
        try {
            out.push_back(nlohmann::json::parse(*v));
        } catch (const std::exception&) {
            // not ours to deliver; dropped (counted by the caller's queue depth only)
        }
    }
    return out;
}

void Store::purge_storage(const StorageRef& s) {
    const std::string pattern = s.prefix() + "*";
    long long cursor = 0;
    do {
        std::vector<std::string> keys;
        cursor = redis_->scan(cursor, pattern, 500, std::back_inserter(keys));
        if (!keys.empty()) {
            redis_->del(keys.begin(), keys.end());
        }
    } while (cursor != 0);
}

} // namespace udsf
