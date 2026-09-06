#include "store.hpp"

#include <nlohmann/json.hpp>

namespace balance_management {

namespace {

using nlohmann::json;

// P4.5/ADR-0060 (E8, Security): real audit trail, same transaction as the mutation it records --
// see bss/product-catalog's own schema.sql header for the full "local per-service table" real
// architectural disclosure this project's service-per-database topology forces.
void write_audit_record(pqxx::work& txn,
                        const std::string& entity_type,
                        const std::string& entity_id,
                        const std::string& action,
                        const std::optional<std::string>& after_snapshot) {
    const auto id = txn.exec("SELECT nextval('audit_record_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    txn.exec("INSERT INTO audit_record (id, entity_type, entity_id, action, actor, "
             "after_snapshot) VALUES ($1,$2,$3,$4,'bss/balance-management',$5::jsonb)",
             pqxx::params{id, entity_type, entity_id, action, after_snapshot});
}

// std::optional, not a bare std::string defaulting to "" -- so libpqxx binds a real SQL NULL for
// an absent ref/name rather than an empty string, which would otherwise round-trip back out as a
// visible (but meaningless) `"name":""` in every response.
std::optional<std::string> party_account_id_of(const std::optional<bss_sid::PartyAccountRef>& ref) {
    return ref.has_value() ? std::optional<std::string>(ref->id) : std::nullopt;
}

std::optional<std::string>
party_account_name_of(const std::optional<bss_sid::PartyAccountRef>& ref) {
    return (ref.has_value() && ref->name.has_value()) ? ref->name : std::nullopt;
}

double amount_value_of(const std::optional<bss_sid::Quantity>& q) {
    return (q.has_value() && q->amount.has_value()) ? *q->amount : 0.0;
}

std::optional<std::string> amount_units_of(const std::optional<bss_sid::Quantity>& q) {
    return (q.has_value() && q->units.has_value()) ? q->units : std::nullopt;
}

std::optional<std::string> valid_from_of(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? vf->startDateTime : std::nullopt;
}

std::optional<std::string> valid_to_of(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? vf->endDateTime : std::nullopt;
}

std::optional<bss_sid::TimePeriod> time_period_from(const std::optional<std::string>& from,
                                                    const std::optional<std::string>& to) {
    if (!from.has_value() && !to.has_value()) {
        return std::nullopt;
    }
    bss_sid::TimePeriod tp;
    tp.startDateTime = from;
    tp.endDateTime = to;
    return tp;
}

template <typename T> std::string dump_array(const std::vector<T>& v) {
    return json(v).dump();
}

template <typename T> std::vector<T> parse_array(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    return json::parse(s).get<std::vector<T>>();
}

template <typename T> std::optional<std::string> dump_optional(const std::optional<T>& v) {
    if (!v.has_value()) {
        return std::nullopt;
    }
    return json(*v).dump();
}

template <typename T> std::optional<T> parse_optional(const std::optional<std::string>& s) {
    if (!s.has_value()) {
        return std::nullopt;
    }
    return json::parse(*s).get<T>();
}

// Templated (not `const pqxx::row&`) because libpqxx 8.x's `pqxx::result::operator[]`/`front()`
// return the lightweight `pqxx::row_ref` view type, while `result::one_row()` returns an owning
// `pqxx::row` -- both support the same named-field `operator[]` access used below, so one
// template serves both call sites (same real, verified fix bss/product-catalog's own store.cpp
// already needed, ADR-0054).
template <typename Row> bss_sid::Bucket row_to_bucket(const Row& row) {
    bss_sid::Bucket v;
    v.id = row["id"].template as<std::optional<std::string>>();
    v.href = row["href"].template as<std::optional<std::string>>();
    v.confirmationDate = row["confirmation_date"].template as<std::optional<std::string>>();
    v.description = row["description"].template as<std::optional<std::string>>();
    v.isShared = row["is_shared"].template as<std::optional<bool>>();
    v.name = row["name"].template as<std::optional<std::string>>();
    v.remainingValueName = row["remaining_value_name"].template as<std::optional<std::string>>();
    v.requestedDate = row["requested_date"].template as<std::optional<std::string>>();

    if (const auto id = row["party_account_id"].template as<std::optional<std::string>>();
        id.has_value()) {
        bss_sid::PartyAccountRef ref{};
        ref.id = *id;
        ref.name = row["party_account_name"].template as<std::optional<std::string>>();
        v.partyAccount = ref;
    }
    v.product = parse_array<bss_sid::ProductRef>(row["product"].template as<std::string>());
    v.logicalResource = parse_array<bss_sid::LogicalResourceRef>(
        row["logical_resource"].template as<std::string>());
    v.relatedParty =
        parse_array<bss_sid::RelatedParty>(row["related_party"].template as<std::string>());

    bss_sid::Money remaining{};
    remaining.unit = row["remaining_value_unit"].template as<std::optional<std::string>>();
    remaining.value = row["remaining_value"].template as<double>();
    v.remainingValue = remaining;

    bss_sid::Money reserved{};
    reserved.unit = row["reserved_value_unit"].template as<std::optional<std::string>>();
    reserved.value = row["reserved_value"].template as<double>();
    v.reservedValue = reserved;

    v.status = row["status"].template as<std::optional<std::string>>();
    v.usageType = row["usage_type"].template as<std::optional<std::string>>();
    return v;
}

} // namespace

BalanceStore::BalanceStore(std::string resource_base_url, const std::string& conninfo)
    : resource_base_url_(std::move(resource_base_url)), conn_(conninfo) {}

std::optional<bss_sid::Bucket> BalanceStore::get_bucket(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM bucket WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_bucket(result.front());
}

std::vector<bss_sid::Bucket> BalanceStore::list_buckets() {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM bucket ORDER BY id");
    std::vector<bss_sid::Bucket> out;
    out.reserve(static_cast<std::size_t>(result.size()));
    for (const auto& row : result) {
        out.push_back(row_to_bucket(row));
    }
    return out;
}

std::optional<bss_sid::Bucket> BalanceStore::find_shared_bucket_for(const std::string& party_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    // Matched in SQL on the real JSONB `related_party` array rather than by scanning every bucket
    // in C++: a subscriber's charging path runs this on every reservation, and a full table scan
    // per request would be a real cost on a real subscriber base.
    //
    // `status` is checked here, not by the caller: an expired or suspended shared bucket must not
    // silently absorb a family's usage, and pushing that check to every call site is how one of
    // them eventually forgets.
    const auto result =
        txn.exec("SELECT * FROM bucket WHERE is_shared = TRUE "
                 "AND related_party @> $1::jsonb "
                 "AND (status IS NULL OR status = 'active') "
                 "ORDER BY id LIMIT 1",
                 pqxx::params{nlohmann::json::array({nlohmann::json{{"id", party_id}}}).dump()});
    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_bucket(result[0]);
}

bss_sid::AccumulatedBalance
BalanceStore::get_accumulated_balance(const std::string& party_account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT id, remaining_value_unit, remaining_value FROM bucket "
                                 "WHERE party_account_id = $1 ORDER BY id",
                                 pqxx::params{party_account_id});

    bss_sid::AccumulatedBalance accumulated{};
    bss_sid::PartyAccountRef account_ref{};
    account_ref.id = party_account_id;
    accumulated.partyAccount = account_ref;

    double total = 0.0;
    std::optional<std::string> unit;
    bool mixed_units = false;
    for (const auto& row : result) {
        bss_sid::BucketRef ref{};
        ref.id = row["id"].as<std::string>();
        accumulated.bucket.push_back(ref);

        const auto row_unit = row["remaining_value_unit"].as<std::optional<std::string>>();
        if (!unit.has_value()) {
            unit = row_unit;
        } else if (row_unit != unit) {
            mixed_units = true;
        }
        total += row["remaining_value"].as<double>();
    }

    bss_sid::Money total_balance{};
    total_balance.unit = unit;
    total_balance.value = total;
    accumulated.totalBalance = total_balance;
    if (mixed_units) {
        // Disclosed simplification (see store.hpp's own comment): a real multi-currency/
        // multi-unit aggregation isn't implemented -- flagged in the response itself rather than
        // silently returning a number that mixes incompatible units.
        accumulated.description =
            "WARNING: this party account's buckets use mixed units/currencies; totalBalance is a "
            "naive sum and not meaningful -- real multi-currency conversion is not implemented.";
    }
    return accumulated;
}

MutationResult<bss_sid::TopupBalance> BalanceStore::topup(bss_sid::TopupBalance request) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);

    const std::string bucket_id = request.bucket->id;
    const double amount = amount_value_of(request.amount);
    const std::optional<std::string> amount_units = amount_units_of(request.amount);

    auto upd = txn.exec("UPDATE bucket SET remaining_value = remaining_value + $1, updated_at = "
                        "now() WHERE id = $2",
                        pqxx::params{amount, bucket_id});

    if (upd.affected_rows() == 0) {
        // Real, disclosed interpretation (see store.hpp): a topup referencing a not-yet-existing
        // bucket creates it, since the real TMF654 API has no POST /bucket at all.
        txn.exec("INSERT INTO bucket (id, href, party_account_id, party_account_name, product, "
                 "remaining_value_unit, remaining_value, usage_type, status) "
                 "VALUES ($1,$2,$3,$4,$5::jsonb,$6,$7,$8,'active')",
                 pqxx::params{bucket_id,
                              resource_base_url_ + "/bucket/" + bucket_id,
                              party_account_id_of(request.partyAccount),
                              party_account_name_of(request.partyAccount),
                              dump_array(request.product),
                              amount_units,
                              amount,
                              request.usageType.value_or("monetary")});
    }

    const auto topup_id = txn.exec("SELECT nextval('topup_balance_id_seq')::text AS id")
                              .one_row()["id"]
                              .as<std::string>();
    request.id = topup_id;
    request.href = resource_base_url_ + "/topupBalance/" + topup_id;
    request.status = "completed";

    txn.exec(
        "INSERT INTO topup_balance (id, href, confirmation_date, description, is_auto_topup, "
        "number_of_periods, reason, requested_date, voucher, bucket_id, amount, amount_units, "
        "balance_topup, channel, logical_resource, party_account_id, payment_method, product, "
        "recurring_period, related_party, requestor, status, usage_type, valid_from, valid_to) "
        "VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13::jsonb,$14::jsonb,$15::jsonb,$16,$17::jsonb,"
        "$18::jsonb,$19,$20::jsonb,$21::jsonb,$22,$23,$24,$25)",
        pqxx::params{topup_id,
                     *request.href,
                     request.confirmationDate,
                     request.description,
                     request.isAutoTopup,
                     request.numberOfPeriods,
                     request.reason,
                     request.requestedDate,
                     request.voucher,
                     bucket_id,
                     amount,
                     amount_units,
                     dump_optional(request.balanceTopup),
                     dump_optional(request.channel),
                     dump_array(request.logicalResource),
                     party_account_id_of(request.partyAccount),
                     dump_optional(request.paymentMethod),
                     dump_array(request.product),
                     request.recurringPeriod,
                     dump_array(request.relatedParty),
                     dump_optional(request.requestor),
                     *request.status,
                     request.usageType,
                     valid_from_of(request.validFor),
                     valid_to_of(request.validFor)});

    write_audit_record(txn, "TOPUP_BALANCE", topup_id, "balance.topup", json(request).dump());
    txn.commit();
    return {request, true};
}

std::optional<bss_sid::TopupBalance> BalanceStore::get_topup(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM topup_balance WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto& row = result.front();
    bss_sid::TopupBalance v{};
    v.id = row["id"].as<std::optional<std::string>>();
    v.href = row["href"].as<std::optional<std::string>>();
    v.description = row["description"].as<std::optional<std::string>>();
    v.isAutoTopup = row["is_auto_topup"].as<std::optional<bool>>();
    v.numberOfPeriods = row["number_of_periods"].as<std::optional<int>>();
    v.reason = row["reason"].as<std::optional<std::string>>();
    v.requestedDate = row["requested_date"].as<std::optional<std::string>>();
    v.voucher = row["voucher"].as<std::optional<std::string>>();
    bss_sid::BucketRef bucket_ref{};
    bucket_ref.id = row["bucket_id"].as<std::string>();
    v.bucket = bucket_ref;
    bss_sid::Quantity amount{};
    amount.amount = row["amount"].as<double>();
    amount.units = row["amount_units"].as<std::optional<std::string>>();
    v.amount = amount;
    v.balanceTopup = parse_optional<bss_sid::RelatedTopupBalance>(
        row["balance_topup"].as<std::optional<std::string>>());
    v.channel =
        parse_optional<bss_sid::ChannelRef>(row["channel"].as<std::optional<std::string>>());
    v.logicalResource =
        parse_array<bss_sid::LogicalResourceRef>(row["logical_resource"].as<std::string>());
    v.paymentMethod = parse_optional<bss_sid::PaymentMethodRef>(
        row["payment_method"].as<std::optional<std::string>>());
    v.product = parse_array<bss_sid::ProductRef>(row["product"].as<std::string>());
    v.recurringPeriod = row["recurring_period"].as<std::optional<std::string>>();
    v.relatedParty = parse_array<bss_sid::RelatedParty>(row["related_party"].as<std::string>());
    v.requestor =
        parse_optional<bss_sid::RelatedParty>(row["requestor"].as<std::optional<std::string>>());
    v.status = row["status"].as<std::optional<std::string>>();
    v.usageType = row["usage_type"].as<std::optional<std::string>>();
    v.validFor = time_period_from(row["valid_from"].as<std::optional<std::string>>(),
                                  row["valid_to"].as<std::optional<std::string>>());
    return v;
}

MutationResult<bss_sid::AdjustBalance> BalanceStore::adjust(bss_sid::AdjustBalance request) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);

    const std::string bucket_id = request.bucket->id;
    const double amount = amount_value_of(request.amount);
    const std::optional<std::string> amount_units = amount_units_of(request.amount);

    // Real atomic compare-and-set: the WHERE clause enforces the balance floor in the SAME
    // statement as the mutation, under PostgreSQL's own row-level lock -- no separate read-then-
    // write race is possible between concurrent callers. This is the real strong-consistency
    // mechanism CHARGING_PROMPT.md's P4.3 asks to be proven under concurrent debit tests.
    auto upd =
        txn.exec("UPDATE bucket SET remaining_value = remaining_value + $1, updated_at = now() "
                 "WHERE id = $2 AND remaining_value + $1 >= 0",
                 pqxx::params{amount, bucket_id});

    const bool succeeded = upd.affected_rows() > 0;

    const auto adjust_id = txn.exec("SELECT nextval('adjust_balance_id_seq')::text AS id")
                               .one_row()["id"]
                               .as<std::string>();
    request.id = adjust_id;
    request.href = resource_base_url_ + "/adjustBalance/" + adjust_id;
    request.status = succeeded ? "completed" : "failed";

    txn.exec("INSERT INTO adjust_balance (id, href, confirmation_date, description, reason, "
             "requested_date, adjust_type, bucket_id, amount, amount_units, channel, "
             "logical_resource, party_account_id, product, related_party, requestor, status, "
             "usage_type, valid_from, valid_to) "
             "VALUES "
             "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11::jsonb,$12::jsonb,$13,$14::jsonb,$15::jsonb,"
             "$16::jsonb,$17,$18,$19,$20)",
             pqxx::params{adjust_id,
                          *request.href,
                          request.confirmationDate,
                          request.description,
                          request.reason,
                          request.requestedDate,
                          request.adjustType,
                          bucket_id,
                          amount,
                          amount_units,
                          dump_optional(request.channel),
                          dump_array(request.logicalResource),
                          party_account_id_of(request.partyAccount),
                          dump_array(request.product),
                          dump_array(request.relatedParty),
                          dump_optional(request.requestor),
                          *request.status,
                          request.usageType,
                          valid_from_of(request.validFor),
                          valid_to_of(request.validFor)});

    write_audit_record(txn, "ADJUST_BALANCE", adjust_id, "balance.adjust", json(request).dump());
    txn.commit();
    return {request, succeeded};
}

std::optional<bss_sid::AdjustBalance> BalanceStore::get_adjust(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM adjust_balance WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto& row = result.front();
    bss_sid::AdjustBalance v{};
    v.id = row["id"].as<std::optional<std::string>>();
    v.href = row["href"].as<std::optional<std::string>>();
    v.description = row["description"].as<std::optional<std::string>>();
    v.reason = row["reason"].as<std::optional<std::string>>();
    v.adjustType = row["adjust_type"].as<std::optional<std::string>>();
    bss_sid::BucketRef bucket_ref{};
    bucket_ref.id = row["bucket_id"].as<std::string>();
    v.bucket = bucket_ref;
    bss_sid::Quantity amount{};
    amount.amount = row["amount"].as<double>();
    amount.units = row["amount_units"].as<std::optional<std::string>>();
    v.amount = amount;
    v.channel =
        parse_optional<bss_sid::ChannelRef>(row["channel"].as<std::optional<std::string>>());
    v.logicalResource =
        parse_array<bss_sid::LogicalResourceRef>(row["logical_resource"].as<std::string>());
    v.product = parse_array<bss_sid::ProductRef>(row["product"].as<std::string>());
    v.relatedParty = parse_array<bss_sid::RelatedParty>(row["related_party"].as<std::string>());
    v.requestor =
        parse_optional<bss_sid::RelatedParty>(row["requestor"].as<std::optional<std::string>>());
    v.status = row["status"].as<std::optional<std::string>>();
    v.usageType = row["usage_type"].as<std::optional<std::string>>();
    v.validFor = time_period_from(row["valid_from"].as<std::optional<std::string>>(),
                                  row["valid_to"].as<std::optional<std::string>>());
    return v;
}

MutationResult<bss_sid::ReserveBalance> BalanceStore::reserve(bss_sid::ReserveBalance request) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);

    const std::string bucket_id = request.bucket->id;
    const double amount = amount_value_of(request.amount);
    const std::optional<std::string> amount_units = amount_units_of(request.amount);

    pqxx::result upd;
    if (amount >= 0) {
        // Reserve: move remainingValue -> reservedValue, atomically, only if enough remains.
        upd = txn.exec("UPDATE bucket SET remaining_value = remaining_value - $1, "
                       "reserved_value = reserved_value + $1, updated_at = now() "
                       "WHERE id = $2 AND remaining_value >= $1",
                       pqxx::params{amount, bucket_id});
    } else {
        // Unreserve/refund (negative amount, this project's disclosed sign convention -- see
        // store.hpp): move |amount| back from reservedValue -> remainingValue, atomically, only
        // if that much is actually reserved.
        const double refund = -amount;
        upd = txn.exec("UPDATE bucket SET remaining_value = remaining_value + $1, "
                       "reserved_value = reserved_value - $1, updated_at = now() "
                       "WHERE id = $2 AND reserved_value >= $1",
                       pqxx::params{refund, bucket_id});
    }

    const bool succeeded = upd.affected_rows() > 0;

    const auto reserve_id = txn.exec("SELECT nextval('reserve_balance_id_seq')::text AS id")
                                .one_row()["id"]
                                .as<std::string>();
    request.id = reserve_id;
    request.href = resource_base_url_ + "/reserveBalance/" + reserve_id;
    request.status = succeeded ? "completed" : "failed";

    txn.exec(
        "INSERT INTO reserve_balance (id, href, confirmation_date, description, reason, "
        "requested_date, bucket_id, amount, amount_units, channel, logical_resource, "
        "party_account_id, product, related_party, requestor, status, usage_type, valid_from, "
        "valid_to) "
        "VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10::jsonb,$11::jsonb,$12,$13::jsonb,$14::jsonb,$15::jsonb,"
        "$16,$17,$18,$19)",
        pqxx::params{reserve_id,
                     *request.href,
                     request.confirmationDate,
                     request.description,
                     request.reason,
                     request.requestedDate,
                     bucket_id,
                     amount,
                     amount_units,
                     dump_optional(request.channel),
                     dump_array(request.logicalResource),
                     party_account_id_of(request.partyAccount),
                     dump_array(request.product),
                     dump_array(request.relatedParty),
                     dump_optional(request.requestor),
                     *request.status,
                     request.usageType,
                     valid_from_of(request.validFor),
                     valid_to_of(request.validFor)});

    write_audit_record(txn, "RESERVE_BALANCE", reserve_id, "balance.reserve", json(request).dump());
    txn.commit();
    return {request, succeeded};
}

std::optional<bss_sid::ReserveBalance> BalanceStore::get_reserve(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pqxx::work txn(conn_);
    const auto result = txn.exec("SELECT * FROM reserve_balance WHERE id = $1", pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto& row = result.front();
    bss_sid::ReserveBalance v{};
    v.id = row["id"].as<std::optional<std::string>>();
    v.href = row["href"].as<std::optional<std::string>>();
    v.description = row["description"].as<std::optional<std::string>>();
    v.reason = row["reason"].as<std::optional<std::string>>();
    bss_sid::BucketRef bucket_ref{};
    bucket_ref.id = row["bucket_id"].as<std::string>();
    v.bucket = bucket_ref;
    bss_sid::Quantity amount{};
    amount.amount = row["amount"].as<double>();
    amount.units = row["amount_units"].as<std::optional<std::string>>();
    v.amount = amount;
    v.channel =
        parse_optional<bss_sid::ChannelRef>(row["channel"].as<std::optional<std::string>>());
    v.logicalResource =
        parse_array<bss_sid::LogicalResourceRef>(row["logical_resource"].as<std::string>());
    v.product = parse_array<bss_sid::ProductRef>(row["product"].as<std::string>());
    v.relatedParty = parse_array<bss_sid::RelatedParty>(row["related_party"].as<std::string>());
    v.requestor =
        parse_optional<bss_sid::RelatedParty>(row["requestor"].as<std::optional<std::string>>());
    v.status = row["status"].as<std::optional<std::string>>();
    v.usageType = row["usage_type"].as<std::optional<std::string>>();
    v.validFor = time_period_from(row["valid_from"].as<std::optional<std::string>>(),
                                  row["valid_to"].as<std::optional<std::string>>());
    return v;
}

} // namespace balance_management
