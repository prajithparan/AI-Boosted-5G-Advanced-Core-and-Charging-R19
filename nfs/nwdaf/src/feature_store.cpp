#include "feature_store.hpp"

#include <spdlog/spdlog.h>

#include <mysql.h>
#include <stdexcept>

namespace nwdaf {

FeatureStore::FeatureStore(const FeatureStoreOptions& o) : max_rows_(o.max_rows) {
    conn_ = mysql_init(nullptr);
    if (conn_ == nullptr) {
        throw std::runtime_error("mysql_init failed");
    }
    // libmariadb 3.4 requires TLS by default and Doris' MySQL port does not offer it, so the
    // handshake fails with "SSL is required, but the server does not support it" unless BOTH
    // options are cleared -- verify-server-cert alone forces use_ssl during auth. Same finding
    // and same two calls as CHF's CdrWriter (nfs/chf/src/cdr.cpp), reached independently here
    // when the first smoke test refused to connect. Lab-only plaintext; a deployment fronting
    // Doris with TLS turns these back on.
    my_bool ssl_enforce = 0;
    mysql_options(conn_, MYSQL_OPT_SSL_ENFORCE, &ssl_enforce);
    my_bool ssl_verify = 0;
    mysql_options(conn_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_verify);
    if (mysql_real_connect(conn_,
                           o.host.c_str(),
                           o.user.c_str(),
                           o.password.c_str(),
                           o.database.c_str(),
                           o.port,
                           nullptr,
                           0) == nullptr) {
        // Degrade, as CHF's writer does: NWDAF still serves NF_LOAD from the NRF, and says so.
        spdlog::warn(
            "nwdaf: feature store unreachable ({}), ABNORMAL_BEHAVIOUR analytics disabled: {}",
            o.host,
            mysql_error(conn_));
        mysql_close(conn_);
        conn_ = nullptr;
        return;
    }
    spdlog::info("nwdaf: connected to the feature store at {}:{}/{}", o.host, o.port, o.database);
}

FeatureStore::~FeatureStore() {
    if (conn_ != nullptr) {
        mysql_close(conn_);
    }
}

namespace {
std::string escape_date(MYSQL* c, const std::string& d) {
    std::string out(d.size() * 2 + 1, '\0');
    const auto n =
        mysql_real_escape_string(c, out.data(), d.c_str(), static_cast<unsigned long>(d.size()));
    out.resize(n);
    return out;
}
double num(const char* v) {
    return v ? std::strtod(v, nullptr) : 0.0;
}
std::int64_t integer(const char* v) {
    return v ? std::strtoll(v, nullptr, 10) : 0;
}
} // namespace

std::vector<FeatureRow> FeatureStore::read_day(const std::string& date) {
    std::vector<FeatureRow> rows;
    if (conn_ == nullptr) {
        return rows;
    }
    const std::string sql =
        "SELECT feature_date, subscriber_identifier, session_count, total_used_octets, "
        "max_session_octets, avg_session_octets, stddev_session_octets, roaming_cdrs "
        "FROM subscriber_features WHERE feature_date = '" +
        escape_date(conn_, date) + "'" +
        (max_rows_ > 0 ? " LIMIT " + std::to_string(max_rows_) : "");
    if (mysql_real_query(conn_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        throw std::runtime_error(std::string("feature store query failed: ") + mysql_error(conn_));
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (res == nullptr) {
        throw std::runtime_error(std::string("feature store result failed: ") + mysql_error(conn_));
    }
    while (MYSQL_ROW r = mysql_fetch_row(res)) {
        FeatureRow row;
        row.feature_date = r[0] ? r[0] : "";
        row.subscriber_identifier = r[1] ? r[1] : "";
        row.session_count = integer(r[2]);
        row.total_used_octets = num(r[3]);
        row.max_session_octets = num(r[4]);
        row.avg_session_octets = num(r[5]);
        row.stddev_session_octets = num(r[6]);
        row.roaming_cdrs = integer(r[7]);
        rows.push_back(std::move(row));
    }
    mysql_free_result(res);
    return rows;
}

std::string FeatureStore::latest_date() {
    if (conn_ == nullptr) {
        return {};
    }
    const char* sql = "SELECT MAX(feature_date) FROM subscriber_features";
    if (mysql_real_query(conn_, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
        throw std::runtime_error(std::string("feature store query failed: ") + mysql_error(conn_));
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    std::string out;
    if (res != nullptr) {
        if (MYSQL_ROW r = mysql_fetch_row(res); r != nullptr && r[0] != nullptr) {
            out = r[0];
        }
        mysql_free_result(res);
    }
    return out;
}

} // namespace nwdaf
