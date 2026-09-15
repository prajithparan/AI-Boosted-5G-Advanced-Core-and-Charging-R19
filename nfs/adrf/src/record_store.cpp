#include "record_store.hpp"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <ctime>
#include <mysql.h>
#include <stdexcept>

namespace adrf {

std::string doris_datetime(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf,
                  sizeof buf,
                  "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900,
                  tm.tm_mon + 1,
                  tm.tm_mday,
                  tm.tm_hour,
                  tm.tm_min,
                  tm.tm_sec);
    return buf;
}

std::chrono::system_clock::time_point parse_doris_datetime(const std::string& text) {
    std::tm tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
        return {};
    }
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

std::string fingerprint(const std::string& kind, const nlohmann::json& spec) {
    // The notification-target fields of every source subscription the DCCF strips (ADR-0366):
    // the same list, so both NFs compute the same key for the same spec. An anaSub is the
    // NnwdafEventsSubscription itself; a dataSub is {"<source>DataSub": {...}} and the fields are
    // stripped inside the one alternative it carries.
    static const char* kNotificationFields[] = {"eventNotifyUri",
                                                "notifyCorrelationId",
                                                "nfId",
                                                "subsChangeNotifyUri",
                                                "subsChangeNotifyCorrelationId",
                                                "nfStatusNotificationUri",
                                                "reqNfInstanceId",
                                                "subscriptionId",
                                                "notificationURI",
                                                "notifCorrId",
                                                "notifUri",
                                                "notifId"};
    nlohmann::json stripped = spec;
    const auto strip = [](nlohmann::json& o) {
        if (!o.is_object()) {
            return;
        }
        for (const char* f : kNotificationFields) {
            o.erase(f);
        }
    };
    strip(stripped);
    if (stripped.is_object()) {
        for (auto& [k, v] : stripped.items()) {
            if (k.ends_with("DataSub")) {
                strip(v);
            }
        }
    }
    // nlohmann::json objects serialise with keys in sorted order, so dump() is canonical.
    const std::string canonical = kind + "|" + stripped.dump();
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : canonical) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

RecordStore::RecordStore(const RecordStoreOptions& o) : options_(o) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ensure_connected_locked();
}

RecordStore::~RecordStore() {
    if (conn_ != nullptr) {
        mysql_close(conn_);
    }
}

bool RecordStore::connected() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return ensure_connected_locked();
}

bool RecordStore::ensure_connected_locked() {
    if (conn_ != nullptr) {
        return true;
    }
    MYSQL* c = mysql_init(nullptr);
    if (c == nullptr) {
        return false;
    }
    // libmariadb 3.4 requires TLS by default and Doris' MySQL port does not offer it (same two
    // options CHF's CdrWriter and NWDAF's FeatureStore clear; lab-only plaintext).
    my_bool ssl_enforce = 0;
    mysql_options(c, MYSQL_OPT_SSL_ENFORCE, &ssl_enforce);
    my_bool ssl_verify = 0;
    mysql_options(c, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_verify);
    if (mysql_real_connect(c,
                           options_.host.c_str(),
                           options_.user.c_str(),
                           options_.password.c_str(),
                           options_.database.c_str(),
                           options_.port,
                           nullptr,
                           0) == nullptr) {
        spdlog::warn("adrf: data store unreachable at {}:{}: {}",
                     options_.host,
                     options_.port,
                     mysql_error(c));
        mysql_close(c);
        return false;
    }
    conn_ = c;
    spdlog::info("adrf: connected to the data store at {}:{}/{}",
                 options_.host,
                 options_.port,
                 options_.database);
    return true;
}

std::string RecordStore::quote_locked(const std::string& s) {
    // mysql_real_escape_string needs a worst-case buffer of 2*len+1.
    std::string out(s.size() * 2 + 1, '\0');
    const auto n = mysql_real_escape_string(
        conn_, out.data(), s.c_str(), static_cast<unsigned long>(s.size()));
    out.resize(n);
    return "'" + out + "'";
}

void RecordStore::exec_locked(const std::string& sql) {
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    if (mysql_real_query(conn_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        const std::string err = mysql_error(conn_);
        const unsigned code = mysql_errno(conn_);
        // CR_SERVER_GONE_ERROR / CR_SERVER_LOST: drop the handle so the next call reconnects.
        if (code == 2006 || code == 2013) {
            mysql_close(conn_);
            conn_ = nullptr;
        }
        throw std::runtime_error("data store statement failed: " + err);
    }
    // Statements without a result set still need the (empty) result consumed.
    if (MYSQL_RES* res = mysql_store_result(conn_); res != nullptr) {
        mysql_free_result(res);
    }
}

namespace {
std::optional<std::string> opt(const char* v) {
    if (v == nullptr) {
        return std::nullopt;
    }
    return std::string(v);
}
} // namespace

std::vector<StoredRecord> RecordStore::select_locked(const std::string& where_clause) {
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    std::string sql =
        "SELECT store_trans_id, kind, origin, spec_fp, data_set_id, collected_at, stored_at, "
        "expires_at, del_notif_uri, del_notif_corr_id, alert_sent, record FROM data_store_records "
        "WHERE " +
        where_clause + " ORDER BY collected_at, store_trans_id";
    if (options_.max_rows > 0) {
        sql += " LIMIT " + std::to_string(options_.max_rows);
    }
    if (mysql_real_query(conn_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        const std::string err = mysql_error(conn_);
        const unsigned code = mysql_errno(conn_);
        if (code == 2006 || code == 2013) {
            mysql_close(conn_);
            conn_ = nullptr;
        }
        throw std::runtime_error("data store query failed: " + err);
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (res == nullptr) {
        throw std::runtime_error(std::string("data store result failed: ") + mysql_error(conn_));
    }
    std::vector<StoredRecord> out;
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        StoredRecord r;
        r.store_trans_id = row[0] ? row[0] : "";
        r.kind = row[1] ? row[1] : "";
        r.origin = row[2] ? row[2] : "";
        r.spec_fp = row[3] ? row[3] : "";
        r.data_set_id = opt(row[4]);
        r.collected_at = parse_doris_datetime(row[5] ? row[5] : "");
        r.stored_at = parse_doris_datetime(row[6] ? row[6] : "");
        r.expires_at = parse_doris_datetime(row[7] ? row[7] : "");
        r.del_notif_uri = opt(row[8]);
        r.del_notif_corr_id = opt(row[9]);
        r.alert_sent = row[10] && (row[10][0] == '1' || row[10][0] == 't');
        try {
            r.record = nlohmann::json::parse(row[11] ? row[11] : "null");
        } catch (const nlohmann::json::parse_error&) {
            r.record = nullptr;
        }
        out.push_back(std::move(r));
    }
    mysql_free_result(res);
    return out;
}

void RecordStore::insert(const StoredRecord& r) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    const auto nullable = [&](const std::optional<std::string>& v) {
        return v ? quote_locked(*v) : std::string("NULL");
    };
    const std::string sql =
        "INSERT INTO data_store_records (store_trans_id, kind, origin, spec_fp, data_set_id, "
        "collected_at, stored_at, expires_at, del_notif_uri, del_notif_corr_id, alert_sent, "
        "record) "
        "VALUES (" +
        quote_locked(r.store_trans_id) + ", " + quote_locked(r.kind) + ", " +
        quote_locked(r.origin) + ", " + quote_locked(r.spec_fp) + ", " + nullable(r.data_set_id) +
        ", " + quote_locked(doris_datetime(r.collected_at)) + ", " +
        quote_locked(doris_datetime(r.stored_at)) + ", " +
        quote_locked(doris_datetime(r.expires_at)) + ", " + nullable(r.del_notif_uri) + ", " +
        nullable(r.del_notif_corr_id) + ", " + (r.alert_sent ? "true" : "false") + ", " +
        quote_locked(r.record.dump()) + ")";
    exec_locked(sql);
}

std::optional<StoredRecord> RecordStore::get(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    auto rows = select_locked("store_trans_id = " + quote_locked(id));
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front();
}

std::vector<StoredRecord> RecordStore::get_many(const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    std::string in;
    for (const auto& id : ids) {
        in += (in.empty() ? "" : ", ") + quote_locked(id);
    }
    return select_locked("store_trans_id IN (" + in + ")");
}

namespace {
std::string window_clause(const std::optional<TimeWindow>& w) {
    if (!w) {
        return "";
    }
    return " AND collected_at >= '" + doris_datetime(w->start) + "' AND collected_at <= '" +
           doris_datetime(w->stop) + "'";
}
} // namespace

std::vector<StoredRecord> RecordStore::by_data_set(const std::string& data_set_id,
                                                   const std::optional<TimeWindow>& window) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    return select_locked("data_set_id = " + quote_locked(data_set_id) + window_clause(window));
}

std::vector<StoredRecord> RecordStore::by_fingerprint(const std::string& spec_fp,
                                                      const std::optional<TimeWindow>& window) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    return select_locked("spec_fp = " + quote_locked(spec_fp) + window_clause(window));
}

bool RecordStore::remove(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    const auto existing = select_locked("store_trans_id = " + quote_locked(id));
    if (existing.empty()) {
        return false;
    }
    exec_locked("DELETE FROM data_store_records WHERE store_trans_id = " + quote_locked(id));
    return true;
}

std::int64_t RecordStore::remove_matching(const std::optional<std::string>& spec_fp,
                                          const std::optional<std::string>& data_set_id,
                                          const TimeWindow& window) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    const std::string where = (spec_fp ? "spec_fp = " + quote_locked(*spec_fp)
                                       : "data_set_id = " + quote_locked(*data_set_id)) +
                              window_clause(window);
    const auto matching = select_locked(where);
    if (matching.empty()) {
        return 0;
    }
    exec_locked("DELETE FROM data_store_records WHERE " + where);
    return static_cast<std::int64_t>(matching.size());
}

std::vector<StoredRecord>
RecordStore::awaiting_alert(std::chrono::system_clock::time_point before) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    return select_locked("alert_sent = false AND del_notif_uri IS NOT NULL AND expires_at <= '" +
                         doris_datetime(before) + "'");
}

void RecordStore::mark_alert_sent(const std::string& id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    exec_locked("UPDATE data_store_records SET alert_sent = true WHERE store_trans_id = " +
                quote_locked(id));
}

void RecordStore::defer_expiry(const std::string& id,
                               std::chrono::system_clock::time_point new_expiry) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    exec_locked("UPDATE data_store_records SET expires_at = '" + doris_datetime(new_expiry) +
                "' WHERE store_trans_id = " + quote_locked(id));
}

std::int64_t RecordStore::remove_expired(std::chrono::system_clock::time_point now) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_connected_locked()) {
        throw std::runtime_error("data store unreachable");
    }
    const std::string where = "expires_at <= '" + doris_datetime(now) + "'";
    const auto expired = select_locked(where);
    if (expired.empty()) {
        return 0;
    }
    exec_locked("DELETE FROM data_store_records WHERE " + where);
    return static_cast<std::int64_t>(expired.size());
}

} // namespace adrf
