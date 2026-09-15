#pragma once

// The ADRF ML Model Store on PostgreSQL (ADR-0367; engine per ADR-0359) -- TS 29.575 5.2's "ADRF
// ML Model Store Records", schema in nfs/adrf/schema.sql. Model bytes live in the database too,
// so every ADRF replica serves every model without a shared volume. One libpqxx connection
// behind a mutex, re-opened after a broken link. Conninfo from config/adrf.json.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pqxx {
class connection;
}

namespace adrf {

struct StoredModel {
    std::int64_t model_unique_id = 0;
    std::string store_trans_id;
    std::optional<std::string> owner_nf_instance_id;
    std::optional<std::string> owner_nf_set_id;
    std::optional<nlohmann::json> source_addr;     // MLModelAddr, when downloaded
    std::optional<nlohmann::json> allow_consumers; // array(AllowedConsumer)
    std::int64_t storage_size = 0;
    std::string bytes; // only filled by get_bytes()
};

class ModelStore {
public:
    explicit ModelStore(const std::string& conninfo);
    ~ModelStore();
    ModelStore(const ModelStore&) = delete;
    ModelStore& operator=(const ModelStore&) = delete;

    bool connected();

    // Every call throws std::runtime_error on a database error.
    void create_record(const std::string& store_trans_id,
                       const std::optional<std::string>& owner_nf_instance_id,
                       const std::optional<std::string>& owner_nf_set_id);
    bool record_exists(const std::string& store_trans_id);
    // Insert-or-replace one model under its record (a PUT re-stores the models it lists).
    void upsert_model(const StoredModel& m, const std::string& bytes);
    std::vector<StoredModel> models_in_record(const std::string& store_trans_id);
    std::vector<StoredModel> models_by_ids(const std::vector<std::int64_t>& ids);
    std::optional<StoredModel> get_bytes(std::int64_t model_unique_id);
    // true when the model existed.
    bool remove_model(std::int64_t model_unique_id);
    // Removes the record and, by cascade, its models; true when the record existed.
    bool remove_record(const std::string& store_trans_id);

private:
    bool ensure_connected_locked();
    std::string conninfo_;
    std::mutex mutex_;
    std::unique_ptr<pqxx::connection> conn_;
};

} // namespace adrf
