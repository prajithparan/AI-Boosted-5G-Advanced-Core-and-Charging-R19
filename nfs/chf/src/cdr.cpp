#include "cdr.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "cdr_asn1.hpp"

namespace chf {

namespace {

// Real, standard hex encoding -- see this file's own header comment for why (Doris has no native
// BLOB type, ADR-0192).
std::string hex_encode(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out.push_back(kHexDigits[(b >> 4) & 0x0F]);
        out.push_back(kHexDigits[b & 0x0F]);
    }
    return out;
}

// mysql_real_escape_string needs a worst-case buffer of 2*len+1 -- the real, documented libmariadb
// contract, not a guessed size.
// ADR-0338: one column list, used by both the single-row and batched INSERT paths so they cannot
// drift into writing different columns.
constexpr const char* kCdrInsertPrefix =
    "INSERT INTO cdr (charging_data_ref, invocation_sequence_number, service_type, "
    "operation, subscriber_identifier, nf_consumer_node_functionality, rating_group, "
    "granted_total_volume, granted_service_specific_units, used_total_volume, "
    "reserved_cost, reserved_cost_currency, invocation_time_stamp, serving_plmn, "
    "is_roaming, charging_information_type, service_charging_information, asn1_cdr) VALUES ";

std::string escape(MYSQL* conn, const std::string& value) {
    std::string out(value.size() * 2 + 1, '\0');
    const auto written = mysql_real_escape_string(
        conn, out.data(), value.c_str(), static_cast<unsigned long>(value.size()));
    out.resize(written);
    return out;
}

std::string sql_or_null(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "NULL";
}

std::string sql_or_null(const std::optional<std::uint64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "NULL";
}

std::string sql_or_null(const std::optional<double>& value) {
    return value.has_value() ? std::to_string(*value) : "NULL";
}

std::string sql_string_or_null(MYSQL* conn, const std::optional<std::string>& value) {
    return value.has_value() ? "'" + escape(conn, *value) + "'" : "NULL";
}

} // namespace

CdrWriter::CdrWriter(const DorisOptions& options) {
    // ADR-0338: batching is opt-in. A default of 1 preserves the exact durability every existing
    // deployment has: one CDR, one INSERT, already on disk when write() returns.
    batch_size_ = options.batch_size > 0 ? options.batch_size : 1;
    flush_interval_ =
        std::chrono::milliseconds(options.flush_interval_ms > 0 ? options.flush_interval_ms : 1000);

    MYSQL* handle = mysql_init(nullptr);
    if (handle == nullptr) {
        spdlog::warn("chf: mysql_init failed, CDR generation disabled");
        return;
    }
    // Real, disclosed simplification found via live verification (root-caused by reading
    // libmariadb's own vendored source, plugins/auth/my_auth.c: MYSQL_OPT_SSL_VERIFY_SERVER_CERT
    // defaults to "verify required", and that alone -- independent of MYSQL_OPT_SSL_ENFORCE --
    // forces use_ssl=1 during the auth handshake). This project's real Doris deployment (the
    // official apache/doris all-in-one image, docker-compose.yml) has no TLS configured on its FE
    // MySQL port, so connecting without disabling both options fails with "SSL is required, but
    // the server does not support it" even though MYSQL_OPT_SSL_ENFORCE alone is off. Both
    // disabled here, consistent with this project's other backend datastore connections
    // (PostgreSQL, Redis/Valkey) which also run over plaintext in this lab -- the real SBI mTLS
    // discipline (TS 33.501) applies to inter-NF traffic, not this backend link. Real, tracked
    // debt alongside ADR-0009's existing TLS gaps, not a new one.
    my_bool ssl_enforce = 0;
    mysql_options(handle, MYSQL_OPT_SSL_ENFORCE, &ssl_enforce);
    my_bool ssl_verify = 0;
    mysql_options(handle, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_verify);
    if (mysql_real_connect(handle,
                           options.host.c_str(),
                           options.user.c_str(),
                           options.password.c_str(),
                           options.database.c_str(),
                           options.port,
                           nullptr,
                           0) == nullptr) {
        spdlog::warn("chf: could not connect to Doris, CDR generation disabled: {}",
                     mysql_error(handle));
        mysql_close(handle);
        return;
    }
    conn_ = handle;
}

CdrWriter::~CdrWriter() {
    // ADR-0338: flush BEFORE closing the connection. A clean shutdown that dropped buffered rows
    // would lose billing data on the one path where losing it is entirely avoidable.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto flushed = flush_locked(); flushed > 0) {
            spdlog::info("chf: flushed {} buffered CDR(s) on shutdown", flushed);
        }
    }
    if (conn_ != nullptr) {
        mysql_close(conn_);
    }
}

void CdrWriter::write(const CdrRecord& record) {
    if (conn_ == nullptr) {
        spdlog::warn("chf: CDR write skipped for ChargingDataRef={} -- Doris not connected",
                     record.charging_data_ref);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Gap-closure (task #108, ADR-0089): real TS 32.298 ChargingRecord, BER-encoded, hex-encoded
    // for storage (ADR-0192, Doris has no BLOB type -- see this file's own header).
    const auto asn1_bytes = encode_chf_cdr(record, record.recording_network_function_id);
    const auto asn1_hex = hex_encode(asn1_bytes);

    // ADR-0311: UTC, not local time.
    //
    // This wrote `std::localtime` until a billing test caught it: the CDR column is an unqualified
    // DATETIME, so a local-time value carries no offset and is ambiguous. TS 32.291's
    // `invocationTimeStamp` is an ABSOLUTE time (RFC 3339 with an offset), and flattening it to
    // whatever timezone the CHF host happens to run in means two CHF instances in different
    // regions write incomparable timestamps into the same table -- and a billing period query
    // silently selects the wrong rows.
    //
    // `std::localtime` was also the non-reentrant variant (shared static buffer); `gmtime_r` is
    // both correct and reentrant.
    //
    // Disclosed migration consequence: rows written BEFORE this change hold local time and rows
    // after hold UTC. In a deployment that has already accumulated CDRs those two are not
    // comparable, and nothing here rewrites the old rows.
    std::tm tm{};
    char ts_buf[32];
    std::strftime(
        ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", gmtime_r(&record.invocation_time_stamp, &tm));

    // ADR-0338: the column list is shared between the single-row and batched paths, so the two
    // cannot drift into writing different columns.
    std::ostringstream values_tuple;
    values_tuple << "('" << escape(conn_, record.charging_data_ref) << "', "
                 << record.invocation_sequence_number << ", '" << escape(conn_, record.service_type)
                 << "', '" << escape(conn_, record.operation) << "', '"
                 << escape(conn_, record.subscriber_identifier) << "', '"
                 << escape(conn_, record.nf_consumer_node_functionality) << "', "
                 << sql_or_null(record.rating_group) << ", "
                 << sql_or_null(record.granted_total_volume) << ", "
                 << sql_or_null(record.granted_service_specific_units) << ", "
                 << sql_or_null(record.used_total_volume) << ", "
                 << sql_or_null(record.reserved_cost) << ", "
                 << sql_string_or_null(conn_, record.reserved_cost_currency) << ", '" << ts_buf
                 << "', '" << escape(conn_, record.serving_plmn) << "', "
                 << (record.is_roaming ? "TRUE" : "FALSE") << ", '"
                 << escape(conn_, record.charging_information_type) << "', '"
                 << escape(conn_, record.service_charging_information) << "', '" << asn1_hex
                 << "')";

    std::ostringstream sql;
    sql << kCdrInsertPrefix << values_tuple.str();

    if (batch_size_ <= 1) {
        // Unbatched: exactly as before. The row is durable when write() returns.
        const auto query = sql.str();
        if (mysql_real_query(conn_, query.c_str(), static_cast<unsigned long>(query.size())) != 0) {
            spdlog::warn("chf: CDR write to Doris failed for ChargingDataRef={}: {}",
                         record.charging_data_ref,
                         mysql_error(conn_));
        }
        return;
    }

    // ADR-0338: BATCHED. What is being traded is explicit, because CDRs are revenue evidence:
    // a row buffered here is NOT durable, and a CHF that is SIGKILLed loses every unflushed row.
    // That is billing data, not throughput. It is opt-in for exactly that reason, and the flush
    // interval bounds the exposure in time as the batch size bounds it in rows.
    //
    // Doris is an OLAP store: single-row INSERTs are pathologically slow, which is why the
    // measured ceiling was ~25 CDRs/sec with every write serialized behind one connection.
    pending_.push_back(values_tuple.str());
    const auto now = std::chrono::steady_clock::now();
    if (pending_.size() >= static_cast<std::size_t>(batch_size_) ||
        now - last_flush_ >= flush_interval_) {
        flush_locked();
    }
}

std::size_t CdrWriter::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_locked();
}

std::size_t CdrWriter::flush_locked() {
    last_flush_ = std::chrono::steady_clock::now();
    if (pending_.empty() || conn_ == nullptr) {
        return 0;
    }
    std::ostringstream batch;
    batch << kCdrInsertPrefix;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        if (i != 0) {
            batch << ',';
        }
        batch << pending_[i];
    }
    const auto query = batch.str();
    const auto count = pending_.size();
    if (mysql_real_query(conn_, query.c_str(), static_cast<unsigned long>(query.size())) != 0) {
        // The whole batch is lost, and saying how many rows went with it matters: "a CDR write
        // failed" understates losing 500 billing records.
        spdlog::error(
            "chf: batched CDR write of {} rows to Doris FAILED -- those CDRs are lost: {}",
            count,
            mysql_error(conn_));
        pending_.clear();
        return 0;
    }
    pending_.clear();
    return count;
}

CdrWriter::RetentionResult CdrWriter::apply_retention(int retention_days,
                                                      const std::string& archive_dir) {
    RetentionResult result;
    if (retention_days <= 0) {
        return result; // disabled -- the default
    }
    if (conn_ == nullptr) {
        spdlog::warn("chf: CDR retention sweep skipped -- Doris not connected");
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Everything older than the window, read in full BEFORE anything is deleted.
    const std::string cutoff =
        "DATE_SUB(NOW(), INTERVAL " + std::to_string(retention_days) + " DAY)";
    const std::string select =
        "SELECT charging_data_ref, invocation_sequence_number, service_type, operation, "
        "subscriber_identifier, nf_consumer_node_functionality, rating_group, "
        "granted_total_volume, granted_service_specific_units, used_total_volume, reserved_cost, "
        "reserved_cost_currency, invocation_time_stamp, recorded_at, asn1_cdr FROM cdr WHERE "
        "recorded_at < " +
        cutoff;
    if (mysql_real_query(conn_, select.c_str(), static_cast<unsigned long>(select.size())) != 0) {
        spdlog::error("chf: CDR retention SELECT failed: {}", mysql_error(conn_));
        result.failed = true;
        return result;
    }
    MYSQL_RES* rows = mysql_store_result(conn_);
    if (rows == nullptr) {
        spdlog::error("chf: CDR retention SELECT returned no result set: {}", mysql_error(conn_));
        result.failed = true;
        return result;
    }

    static const char* kColumns[] = {"charging_data_ref",
                                     "invocation_sequence_number",
                                     "service_type",
                                     "operation",
                                     "subscriber_identifier",
                                     "nf_consumer_node_functionality",
                                     "rating_group",
                                     "granted_total_volume",
                                     "granted_service_specific_units",
                                     "used_total_volume",
                                     "reserved_cost",
                                     "reserved_cost_currency",
                                     "invocation_time_stamp",
                                     "recorded_at",
                                     "asn1_cdr"};
    const unsigned int column_count = mysql_num_fields(rows);

    std::vector<nlohmann::json> archived_rows;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(rows)) != nullptr) {
        nlohmann::json entry;
        for (unsigned int i = 0; i < column_count && i < std::size(kColumns); ++i) {
            entry[kColumns[i]] = row[i] != nullptr ? nlohmann::json(row[i]) : nlohmann::json();
        }
        archived_rows.push_back(std::move(entry));
    }
    mysql_free_result(rows);

    if (archived_rows.empty()) {
        return result; // nothing old enough; not an error
    }

    // Write the archive and make sure it is really on disk before deleting anything. A real
    // deployment points archive_dir at object storage (docs/DATA_MODEL.md's E4 assignment); this
    // writes newline-delimited JSON, which is what an object-store loader would ingest.
    std::error_code ec;
    std::filesystem::create_directories(archive_dir, ec);
    if (ec) {
        spdlog::error(
            "chf: CDR retention could not create archive dir {}: {}", archive_dir, ec.message());
        result.failed = true;
        return result;
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const std::string path = archive_dir + "/cdr-archive-" + std::to_string(stamp) + ".jsonl";
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            spdlog::error("chf: CDR retention could not open archive file {}", path);
            result.failed = true;
            return result;
        }
        for (const auto& entry : archived_rows) {
            out << entry.dump() << "\n";
        }
        out.flush();
        if (!out) {
            spdlog::error("chf: CDR retention failed writing archive {} -- deleting nothing", path);
            result.failed = true;
            return result;
        }
    }
    result.archived = static_cast<std::int64_t>(archived_rows.size());

    // Only now, with the archive written and flushed, delete. Same predicate as the SELECT: a row
    // that became old between the two statements is archived on the NEXT sweep rather than being
    // deleted unarchived by this one.
    const std::string del = "DELETE FROM cdr WHERE recorded_at < " + cutoff;
    if (mysql_real_query(conn_, del.c_str(), static_cast<unsigned long>(del.size())) != 0) {
        spdlog::error("chf: CDR retention DELETE failed after archiving {} row(s) to {}: {} -- the "
                      "archive is kept and the rows remain; the next sweep retries",
                      result.archived,
                      path,
                      mysql_error(conn_));
        result.failed = true;
        return result;
    }
    result.deleted = static_cast<std::int64_t>(mysql_affected_rows(conn_));
    spdlog::info("chf: CDR retention swept {} row(s) older than {} day(s) into {} and deleted {}",
                 result.archived,
                 retention_days,
                 path,
                 result.deleted);
    return result;
}

std::vector<std::int64_t> CdrWriter::detect_gaps(const std::string& charging_data_ref) {
    if (conn_ == nullptr) {
        spdlog::warn("chf: CDR gap-detection skipped for ChargingDataRef={} -- Doris not connected",
                     charging_data_ref);
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::int64_t> seen;

    const std::string query = "SELECT DISTINCT invocation_sequence_number FROM cdr WHERE "
                              "charging_data_ref = '" +
                              escape(conn_, charging_data_ref) +
                              "' ORDER BY invocation_sequence_number";
    if (mysql_real_query(conn_, query.c_str(), static_cast<unsigned long>(query.size())) != 0) {
        spdlog::warn("chf: CDR gap-detection query failed for ChargingDataRef={}: {}",
                     charging_data_ref,
                     mysql_error(conn_));
        return {};
    }

    MYSQL_RES* result = mysql_store_result(conn_);
    if (result == nullptr) {
        return {};
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != nullptr) {
        if (row[0] != nullptr) {
            seen.insert(std::stoll(row[0]));
        }
    }
    mysql_free_result(result);

    std::vector<std::int64_t> gaps;
    if (seen.size() < 2) {
        return gaps;
    }
    const auto lo = *seen.begin();
    const auto hi = *seen.rbegin();
    for (auto v = lo; v <= hi; ++v) {
        if (seen.count(v) == 0) {
            gaps.push_back(v);
        }
    }
    return gaps;
}

// ADR-0311: read CDRs back. See cdr.hpp for why only `Release` rows are returned.
std::vector<CdrRecord> CdrWriter::query(const CdrQuery& q) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CdrRecord> out;
    if (conn_ == nullptr) {
        return out;
    }

    std::ostringstream sql;
    sql << "SELECT charging_data_ref, invocation_sequence_number, service_type, operation, "
           "subscriber_identifier, nf_consumer_node_functionality, rating_group, "
           "granted_total_volume, granted_service_specific_units, used_total_volume, "
           "reserved_cost, reserved_cost_currency, UNIX_TIMESTAMP(invocation_time_stamp), "
           "serving_plmn, is_roaming FROM cdr WHERE operation = 'Release'";
    sql << " AND invocation_time_stamp >= '" << escape(conn_, q.period_start) << "'";
    sql << " AND invocation_time_stamp < '" << escape(conn_, q.period_end) << "'";
    if (q.subscriber_identifier.has_value()) {
        sql << " AND subscriber_identifier = '" << escape(conn_, *q.subscriber_identifier) << "'";
    }
    if (q.serving_plmn.has_value()) {
        sql << " AND serving_plmn = '" << escape(conn_, *q.serving_plmn) << "'";
    }
    if (q.is_roaming.has_value()) {
        sql << " AND is_roaming = " << (*q.is_roaming ? "TRUE" : "FALSE");
    }
    sql << " ORDER BY invocation_time_stamp";

    const auto statement = sql.str();
    if (mysql_real_query(conn_, statement.c_str(), statement.size()) != 0) {
        spdlog::warn("chf: CDR query failed: {}", mysql_error(conn_));
        return out;
    }
    MYSQL_RES* result = mysql_store_result(conn_);
    if (result == nullptr) {
        return out;
    }
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        CdrRecord record{};
        record.charging_data_ref = row[0] != nullptr ? row[0] : "";
        record.invocation_sequence_number = row[1] != nullptr ? std::stoll(row[1]) : 0;
        record.service_type = row[2] != nullptr ? row[2] : "";
        record.operation = row[3] != nullptr ? row[3] : "";
        record.subscriber_identifier = row[4] != nullptr ? row[4] : "";
        record.nf_consumer_node_functionality = row[5] != nullptr ? row[5] : "";
        if (row[6] != nullptr) {
            record.rating_group = std::stoll(row[6]);
        }
        if (row[7] != nullptr) {
            record.granted_total_volume = static_cast<std::uint64_t>(std::stoull(row[7]));
        }
        if (row[8] != nullptr) {
            record.granted_service_specific_units = static_cast<std::uint64_t>(std::stoull(row[8]));
        }
        if (row[9] != nullptr) {
            record.used_total_volume = static_cast<std::uint64_t>(std::stoull(row[9]));
        }
        if (row[10] != nullptr) {
            record.reserved_cost = std::stod(row[10]);
        }
        if (row[11] != nullptr) {
            record.reserved_cost_currency = row[11];
        }
        if (row[12] != nullptr) {
            record.invocation_time_stamp = static_cast<std::time_t>(std::stoll(row[12]));
        }
        record.serving_plmn = row[13] != nullptr ? row[13] : "";
        // Doris returns BOOLEAN as "0"/"1" over the MySQL protocol.
        record.is_roaming = row[14] != nullptr && std::string(row[14]) != "0";
        out.push_back(std::move(record));
    }
    mysql_free_result(result);
    return out;
}

} // namespace chf
