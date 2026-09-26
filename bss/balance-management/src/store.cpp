#include "store.hpp"

#include "sbi_core/datetime.hpp"

#include <nlohmann/json.hpp>

#include <map>

// ADR-0385: TMF654 balance management persisted in schema balance_mgmt of the consolidated charging
// DB (deploy/db/charging/40-balance.sql made lossless and NULL-safe by 41-balance-lossless.sql),
// replacing the per-service JSONB rows.
//
// Access paths:
//   * Bucket (read and mutated on every charge) is normalized: scalar columns + ordered child
//   tables
//     for logicalResource / product / relatedParty.
//   * Balance events (topup/adjust/reserve) are append-only rows in RANGE(occurred_at) partitions;
//     their TMF654 reference lists stay JSONB on the event row -- one INSERT per event on the
//     hottest write path, never a child-table fan-out.
//
// Mutation semantics are unchanged from the per-service store: every balance movement is ONE
// conditional UPDATE (`WHERE remaining >= amount`), so PostgreSQL row locking alone keeps
// concurrent reserves from overdrawing; an insufficient balance is a business outcome (status
// "failed"), not an error. Top-up's implicit bucket creation is now a single atomic upsert (no
// create race).
//
// Date-times are stored as TIMESTAMPTZ and returned as UTC RFC 3339 with milliseconds (the
// sbi_core::format_rfc3339 form). Every statement is schema-qualified.

namespace balance_management {

namespace {

using nlohmann::json;

constexpr const char* kActor = "bss/balance-management";

std::string ts(const std::string& col) {
    return "to_char(" + col + " AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"')";
}

std::optional<std::string> ts_in(const std::optional<std::string>& v, const char* field) {
    if (!v.has_value()) {
        return std::nullopt;
    }
    const auto tp = sbi_core::parse_rfc3339(*v);
    if (!tp.has_value()) {
        throw InvalidRequest(std::string(field) + " is not an RFC 3339 date-time");
    }
    return sbi_core::format_rfc3339(*tp);
}

std::optional<std::string> vf_start(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? ts_in(vf->startDateTime, "validFor.startDateTime") : std::nullopt;
}
std::optional<std::string> vf_end(const std::optional<bss_sid::TimePeriod>& vf) {
    return vf.has_value() ? ts_in(vf->endDateTime, "validFor.endDateTime") : std::nullopt;
}

template <typename Row> std::optional<std::string> s(const Row& row, const char* col) {
    return row[col].template as<std::optional<std::string>>();
}

template <typename Row> std::optional<bss_sid::TimePeriod> vf_out(const Row& row) {
    auto a = s(row, "vf_start");
    auto b = s(row, "vf_end");
    if (!a && !b) {
        return std::nullopt;
    }
    return bss_sid::TimePeriod{std::move(a), std::move(b)};
}

template <typename T> std::optional<std::string> j_opt(const std::optional<T>& v) {
    return v.has_value() ? std::optional<std::string>(json(*v).dump()) : std::nullopt;
}
template <typename T> std::optional<std::string> j_arr(const std::vector<T>& v) {
    return v.empty() ? std::nullopt : std::optional<std::string>(json(v).dump());
}
template <typename T, typename Row> std::optional<T> j_opt_out(const Row& row, const char* col) {
    const auto raw = s(row, col);
    return raw.has_value() ? std::optional<T>(json::parse(*raw).template get<T>()) : std::nullopt;
}
template <typename T, typename Row> std::vector<T> j_arr_out(const Row& row, const char* col) {
    const auto raw = s(row, col);
    return raw.has_value() ? json::parse(*raw).template get<std::vector<T>>() : std::vector<T>{};
}

double amount_value_of(const std::optional<bss_sid::Quantity>& q) {
    return (q.has_value() && q->amount.has_value()) ? *q->amount : 0.0;
}
std::optional<std::string> amount_units_of(const std::optional<bss_sid::Quantity>& q) {
    return q.has_value() ? q->units : std::nullopt;
}
std::optional<std::string> account_id(const std::optional<bss_sid::PartyAccountRef>& a) {
    return a.has_value() ? std::optional<std::string>(a->id) : std::nullopt;
}

template <typename Row> std::optional<bss_sid::Quantity> amount_out(const Row& row) {
    auto v = row["amount_value"].template as<std::optional<double>>();
    auto u = s(row, "amount_units");
    if (!v && !u) {
        return std::nullopt;
    }
    return bss_sid::Quantity{v, std::move(u)};
}

template <typename Row> std::optional<bss_sid::BucketRef> bucket_ref_out(const Row& row) {
    auto id = s(row, "bucket_id");
    if (!id.has_value()) {
        return std::nullopt;
    }
    bss_sid::BucketRef ref{};
    ref.id = *id;
    return ref;
}

void write_audit(pqxx::work& txn,
                 const std::string& entity_type,
                 const std::string& entity_id,
                 const std::string& action,
                 const std::optional<std::string>& after) {
    const auto id = txn.exec("SELECT nextval('balance_mgmt.audit_record_id_seq')::text AS id")
                        .one_row()["id"]
                        .as<std::string>();
    txn.exec("INSERT INTO balance_mgmt.audit_record (id, entity_type, entity_id, action, actor, "
             "after_snapshot) VALUES ($1,$2,$3,$4,$5,$6::jsonb)",
             pqxx::params{id, entity_type, entity_id, action, kActor, after});
}

std::string next_id(pqxx::work& txn, const char* seq) {
    return txn.exec(std::string("SELECT nextval('balance_mgmt.") + seq + "')::text AS id")
        .one_row()["id"]
        .as<std::string>();
}

// ---- Bucket reads (set-based: one query per table, whether one bucket or all) -------------------

const std::string kBucketCols =
    "b.id, b.href, b.name, b.description, b.is_shared, b.remaining_value_name, " +
    ts("b.confirmation_date") + " AS confirmation_date, " + ts("b.requested_date") +
    " AS requested_date, b.party_account_id, b.party_account_name, b.party_account_href, "
    "b.party_account_description, b.party_account_status, b.remaining_value_unit, "
    "b.remaining_value_amount::float8 AS remaining_value_amount, b.reserved_value_unit, "
    "b.reserved_value_amount::float8 AS reserved_value_amount, b.status, b.usage_type, " +
    ts("b.valid_for_start") + " AS vf_start, " + ts("b.valid_for_end") + " AS vf_end";

std::vector<bss_sid::Bucket>
load_buckets(pqxx::work& txn, const std::string& where_sql, const pqxx::params& params) {
    std::vector<bss_sid::Bucket> out;
    std::map<std::string, std::size_t> idx;
    for (auto r :
         txn.exec("SELECT " + kBucketCols + " FROM balance_mgmt.bucket b " + where_sql, params)) {
        bss_sid::Bucket b;
        b.id = s(r, "id");
        b.href = s(r, "href");
        b.name = s(r, "name");
        b.description = s(r, "description");
        b.isShared = r["is_shared"].as<std::optional<bool>>();
        b.remainingValueName = s(r, "remaining_value_name");
        b.confirmationDate = s(r, "confirmation_date");
        b.requestedDate = s(r, "requested_date");
        if (auto acc = s(r, "party_account_id"); acc.has_value()) {
            b.partyAccount = bss_sid::PartyAccountRef{*acc,
                                                      s(r, "party_account_href"),
                                                      s(r, "party_account_description"),
                                                      s(r, "party_account_name"),
                                                      s(r, "party_account_status")};
        }
        b.remainingValue =
            bss_sid::Money{s(r, "remaining_value_unit"), r["remaining_value_amount"].as<double>()};
        b.reservedValue =
            bss_sid::Money{s(r, "reserved_value_unit"), r["reserved_value_amount"].as<double>()};
        b.status = s(r, "status");
        b.usageType = s(r, "usage_type");
        b.validFor = vf_out(r);
        idx[*b.id] = out.size();
        out.push_back(std::move(b));
    }
    if (out.empty()) {
        return out;
    }
    // Children of exactly the buckets loaded above: the parent's own filter, as a subquery.
    const std::string in_parents =
        " WHERE bucket_id IN (SELECT b.id FROM balance_mgmt.bucket b " + where_sql + ")";
    for (auto r : txn.exec(
             "SELECT bucket_id, resource_id, href, name FROM balance_mgmt.bucket_logical_resource" +
                 in_parents + " ORDER BY bucket_id, ordinal",
             params)) {
        out[idx.at(r["bucket_id"].as<std::string>())].logicalResource.push_back(
            {r["resource_id"].as<std::string>(), s(r, "href"), s(r, "name")});
    }
    for (auto r :
         txn.exec("SELECT bucket_id, product_id, href, name FROM balance_mgmt.bucket_product" +
                      in_parents + " ORDER BY bucket_id, ordinal",
                  params)) {
        out[idx.at(r["bucket_id"].as<std::string>())].product.push_back(
            {r["product_id"].as<std::string>(), s(r, "href"), s(r, "name")});
    }
    for (auto r : txn.exec(
             "SELECT bucket_id, party_id, href, name, role FROM balance_mgmt.bucket_related_party" +
                 in_parents + " ORDER BY bucket_id, ordinal, id",
             params)) {
        out[idx.at(r["bucket_id"].as<std::string>())].relatedParty.push_back(
            {r["party_id"].as<std::string>(), s(r, "href"), s(r, "name"), s(r, "role")});
    }
    return out;
}

// Columns every balance event row shares, for the three get_* reads.
const std::string kEventCols = "id, href, description, bucket_id, party_account::text AS "
                               "party_account, amount_value::float8 AS amount_value, "
                               "amount_units, channel::text AS channel, logical_resource::text AS "
                               "logical_resource, product::text AS product, related_party::text AS "
                               "related_party, requestor::text AS requestor, status, usage_type, " +
                               ts("confirmation_date") + " AS confirmation_date, " +
                               ts("requested_date") + " AS requested_date, " +
                               ts("valid_for_start") + " AS vf_start, " + ts("valid_for_end") +
                               " AS vf_end";

template <typename Event, typename Row> void fill_event_common(Event& v, const Row& r) {
    v.id = s(r, "id");
    v.href = s(r, "href");
    v.description = s(r, "description");
    v.confirmationDate = s(r, "confirmation_date");
    v.requestedDate = s(r, "requested_date");
    v.amount = amount_out(r);
    v.bucket = bucket_ref_out(r);
    v.channel = j_opt_out<bss_sid::ChannelRef>(r, "channel");
    v.logicalResource = j_arr_out<bss_sid::LogicalResourceRef>(r, "logical_resource");
    v.partyAccount = j_opt_out<bss_sid::PartyAccountRef>(r, "party_account");
    v.product = j_arr_out<bss_sid::ProductRef>(r, "product");
    v.relatedParty = j_arr_out<bss_sid::RelatedParty>(r, "related_party");
    v.requestor = j_opt_out<bss_sid::RelatedParty>(r, "requestor");
    v.status = s(r, "status");
    v.usageType = s(r, "usage_type");
    v.validFor = vf_out(r);
}

} // namespace

BalanceStore::BalanceStore(std::string resource_base_url,
                           const std::string& conninfo,
                           std::size_t pool_size)
    : resource_base_url_(std::move(resource_base_url)), pool_(conninfo, pool_size) {}

std::optional<bss_sid::Bucket> BalanceStore::get_bucket(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto all = load_buckets(txn, "WHERE b.id = $1", pqxx::params{id});
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

std::vector<bss_sid::Bucket> BalanceStore::list_buckets() {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    return load_buckets(txn, "ORDER BY b.id", pqxx::params{});
}

// ADR-0307: the shared (family) bucket a party draws from, if any. Indexed on
// bucket_related_party.party_id (41-balance-lossless.sql): the CHF asks this before every reserve.
std::optional<bss_sid::Bucket> BalanceStore::find_shared_bucket_for(const std::string& party_id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    auto all = load_buckets(
        txn,
        "WHERE b.is_shared AND (b.status IS NULL OR b.status = 'active') AND EXISTS (SELECT 1 FROM "
        "balance_mgmt.bucket_related_party rp WHERE rp.bucket_id = b.id AND rp.party_id = $1) "
        "ORDER BY b.id LIMIT 1",
        pqxx::params{party_id});
    if (all.empty()) {
        return std::nullopt;
    }
    return std::move(all.front());
}

bss_sid::AccumulatedBalance
BalanceStore::get_accumulated_balance(const std::string& party_account_id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result =
        txn.exec("SELECT id, remaining_value_unit, remaining_value_amount::float8 "
                 "AS remaining FROM balance_mgmt.bucket WHERE party_account_id = $1 "
                 "ORDER BY id",
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
        total += row["remaining"].as<double>();
    }
    accumulated.totalBalance = bss_sid::Money{unit, total};
    if (mixed_units) {
        accumulated.description =
            "WARNING: this party account's buckets use mixed units/currencies; totalBalance is a "
            "naive sum and not meaningful -- real multi-currency conversion is not implemented.";
    }
    return accumulated;
}

MutationResult<bss_sid::TopupBalance> BalanceStore::topup(bss_sid::TopupBalance request) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());

    const std::string bucket_id = request.bucket->id;
    const double amount = amount_value_of(request.amount);
    const auto units = amount_units_of(request.amount);
    const auto& acc = request.partyAccount;

    // One atomic statement: credit an existing bucket, or create it (TMF654 has no POST /bucket;
    // a top-up of an unknown bucket is the creation path -- store.hpp). xmax = 0 marks the insert.
    // ADR-0386: bucket.party_account_id references subscriber_mgmt.account again; an unknown
    // account is the caller's error (400), not a 500.
    bool created = false;
    try {
        created =
            txn.exec("INSERT INTO balance_mgmt.bucket (id, href, party_account_id, "
                     "party_account_name, "
                     "party_account_href, party_account_description, party_account_status, "
                     "remaining_value_unit, remaining_value_amount, usage_type, status) VALUES "
                     "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,'active') ON CONFLICT (id) DO UPDATE SET "
                     "remaining_value_amount = balance_mgmt.bucket.remaining_value_amount + "
                     "EXCLUDED.remaining_value_amount, updated_at = now() RETURNING (xmax = 0) AS "
                     "ins",
                     pqxx::params{bucket_id,
                                  resource_base_url_ + "/bucket/" + bucket_id,
                                  account_id(acc),
                                  acc ? acc->name : std::nullopt,
                                  acc ? acc->href : std::nullopt,
                                  acc ? acc->description : std::nullopt,
                                  acc ? acc->status : std::nullopt,
                                  units,
                                  amount,
                                  request.usageType.value_or("monetary")})
                .one_row()["ins"]
                .as<bool>();
    } catch (const pqxx::foreign_key_violation&) {
        throw InvalidRequest("partyAccount " + account_id(acc).value_or("") +
                             " does not exist (subscriber_mgmt.account)");
    }
    if (created) {
        for (std::size_t i = 0; i < request.product.size(); ++i) {
            const auto& p = request.product[i];
            txn.exec("INSERT INTO balance_mgmt.bucket_product (bucket_id, product_id, href, name, "
                     "ordinal) VALUES ($1,$2,$3,$4,$5) ON CONFLICT DO NOTHING",
                     pqxx::params{bucket_id, p.id, p.href, p.name, static_cast<int>(i)});
        }
    }

    const auto topup_id = next_id(txn, "topup_balance_id_seq");
    request.id = topup_id;
    request.href = resource_base_url_ + "/topupBalance/" + topup_id;
    request.status = "completed";
    txn.exec("INSERT INTO balance_mgmt.topup_balance (id, href, description, bucket_id, "
             "party_account_id, party_account, is_auto_topup, number_of_periods, reason, voucher, "
             "amount_value, amount_units, channel_id, channel, payment_method_id, payment_method, "
             "balance_topup, logical_resource, product, related_party, requestor, "
             "recurring_period, status, usage_type, confirmation_date, requested_date, "
             "valid_for_start, valid_for_end) VALUES ($1,$2,$3,$4,$5,$6::jsonb,$7,$8,$9,$10,$11,"
             "$12,$13,$14::jsonb,$15,$16::jsonb,$17::jsonb,$18::jsonb,$19::jsonb,$20::jsonb,"
             "$21::jsonb,$22,$23,$24,$25::timestamptz,$26::timestamptz,$27::timestamptz,"
             "$28::timestamptz)",
             pqxx::params{topup_id,
                          request.href,
                          request.description,
                          bucket_id,
                          account_id(acc),
                          j_opt(acc),
                          request.isAutoTopup,
                          request.numberOfPeriods,
                          request.reason,
                          request.voucher,
                          amount,
                          units,
                          request.channel ? std::optional(request.channel->id) : std::nullopt,
                          j_opt(request.channel),
                          request.paymentMethod ? std::optional(request.paymentMethod->id)
                                                : std::nullopt,
                          j_opt(request.paymentMethod),
                          j_opt(request.balanceTopup),
                          j_arr(request.logicalResource),
                          j_arr(request.product),
                          j_arr(request.relatedParty),
                          j_opt(request.requestor),
                          request.recurringPeriod,
                          request.status,
                          request.usageType,
                          ts_in(request.confirmationDate, "confirmationDate"),
                          ts_in(request.requestedDate, "requestedDate"),
                          vf_start(request.validFor),
                          vf_end(request.validFor)});
    write_audit(txn, "TOPUP_BALANCE", topup_id, "balance.topup", json(request).dump());
    txn.commit();
    return {request, true};
}

std::optional<bss_sid::TopupBalance> BalanceStore::get_topup(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("SELECT " + kEventCols +
                                     ", is_auto_topup, number_of_periods, reason, voucher, "
                                     "payment_method::text AS payment_method, balance_topup::text "
                                     "AS balance_topup, recurring_period FROM "
                                     "balance_mgmt.topup_balance WHERE id = $1",
                                 pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto r = result.front();
    bss_sid::TopupBalance v{};
    fill_event_common(v, r);
    v.isAutoTopup = r["is_auto_topup"].as<std::optional<bool>>();
    v.numberOfPeriods = r["number_of_periods"].as<std::optional<int>>();
    v.reason = s(r, "reason");
    v.voucher = s(r, "voucher");
    v.paymentMethod = j_opt_out<bss_sid::PaymentMethodRef>(r, "payment_method");
    v.balanceTopup = j_opt_out<bss_sid::RelatedTopupBalance>(r, "balance_topup");
    v.recurringPeriod = s(r, "recurring_period");
    return v;
}

MutationResult<bss_sid::AdjustBalance> BalanceStore::adjust(bss_sid::AdjustBalance request) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());

    const std::string bucket_id = request.bucket->id;
    const double amount = amount_value_of(request.amount);
    const auto units = amount_units_of(request.amount);

    // Signed adjustment, never below zero -- one conditional UPDATE (row lock = concurrency
    // safety).
    const bool succeeded =
        txn.exec("UPDATE balance_mgmt.bucket SET remaining_value_amount = remaining_value_amount + "
                 "$1, updated_at = now() WHERE id = $2 AND remaining_value_amount + $1 >= 0",
                 pqxx::params{amount, bucket_id})
            .affected_rows() > 0;

    const auto adjust_id = next_id(txn, "adjust_balance_id_seq");
    request.id = adjust_id;
    request.href = resource_base_url_ + "/adjustBalance/" + adjust_id;
    request.status = succeeded ? "completed" : "failed";
    txn.exec("INSERT INTO balance_mgmt.adjust_balance (id, href, description, bucket_id, "
             "party_account_id, party_account, reason, adjust_type, amount_value, amount_units, "
             "channel, logical_resource, product, related_party, requestor, status, usage_type, "
             "confirmation_date, requested_date, valid_for_start, valid_for_end) VALUES ($1,$2,$3,"
             "$4,$5,$6::jsonb,$7,$8,$9,$10,$11::jsonb,$12::jsonb,$13::jsonb,$14::jsonb,$15::jsonb,"
             "$16,$17,$18::timestamptz,$19::timestamptz,$20::timestamptz,$21::timestamptz)",
             pqxx::params{adjust_id,
                          request.href,
                          request.description,
                          bucket_id,
                          account_id(request.partyAccount),
                          j_opt(request.partyAccount),
                          request.reason,
                          request.adjustType,
                          amount,
                          units,
                          j_opt(request.channel),
                          j_arr(request.logicalResource),
                          j_arr(request.product),
                          j_arr(request.relatedParty),
                          j_opt(request.requestor),
                          request.status,
                          request.usageType,
                          ts_in(request.confirmationDate, "confirmationDate"),
                          ts_in(request.requestedDate, "requestedDate"),
                          vf_start(request.validFor),
                          vf_end(request.validFor)});
    write_audit(txn, "ADJUST_BALANCE", adjust_id, "balance.adjust", json(request).dump());
    txn.commit();
    return {request, succeeded};
}

std::optional<bss_sid::AdjustBalance> BalanceStore::get_adjust(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("SELECT " + kEventCols +
                                     ", reason, adjust_type FROM balance_mgmt.adjust_balance "
                                     "WHERE id = $1",
                                 pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto r = result.front();
    bss_sid::AdjustBalance v{};
    fill_event_common(v, r);
    v.reason = s(r, "reason");
    v.adjustType = s(r, "adjust_type");
    return v;
}

MutationResult<bss_sid::ReserveBalance> BalanceStore::reserve(bss_sid::ReserveBalance request) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());

    const std::string bucket_id = request.bucket->id;
    const double amount = amount_value_of(request.amount);
    const auto units = amount_units_of(request.amount);

    pqxx::result upd;
    if (amount >= 0) {
        // Reserve: remaining -> reserved, atomically, only if enough remains.
        upd = txn.exec("UPDATE balance_mgmt.bucket SET remaining_value_amount = "
                       "remaining_value_amount - $1, reserved_value_amount = reserved_value_amount "
                       "+ $1, updated_at = now() WHERE id = $2 AND remaining_value_amount >= $1",
                       pqxx::params{amount, bucket_id});
    } else {
        // Unreserve/refund (negative amount, the disclosed sign convention in store.hpp): reserved
        // -> remaining, atomically, only if that much is actually reserved.
        const double refund = -amount;
        upd = txn.exec("UPDATE balance_mgmt.bucket SET remaining_value_amount = "
                       "remaining_value_amount + $1, reserved_value_amount = reserved_value_amount "
                       "- $1, updated_at = now() WHERE id = $2 AND reserved_value_amount >= $1",
                       pqxx::params{refund, bucket_id});
    }
    const bool succeeded = upd.affected_rows() > 0;

    const auto reserve_id = next_id(txn, "reserve_balance_id_seq");
    request.id = reserve_id;
    request.href = resource_base_url_ + "/reserveBalance/" + reserve_id;
    request.status = succeeded ? "completed" : "failed";
    txn.exec("INSERT INTO balance_mgmt.reserve_balance (id, href, description, bucket_id, "
             "party_account_id, party_account, reason, amount_value, amount_units, channel, "
             "logical_resource, product, related_party, requestor, status, usage_type, "
             "confirmation_date, requested_date, valid_for_start, valid_for_end) VALUES ($1,$2,$3,"
             "$4,$5,$6::jsonb,$7,$8,$9,$10::jsonb,$11::jsonb,$12::jsonb,$13::jsonb,$14::jsonb,$15,"
             "$16,$17::timestamptz,$18::timestamptz,$19::timestamptz,$20::timestamptz)",
             pqxx::params{reserve_id,
                          request.href,
                          request.description,
                          bucket_id,
                          account_id(request.partyAccount),
                          j_opt(request.partyAccount),
                          request.reason,
                          amount,
                          units,
                          j_opt(request.channel),
                          j_arr(request.logicalResource),
                          j_arr(request.product),
                          j_arr(request.relatedParty),
                          j_opt(request.requestor),
                          request.status,
                          request.usageType,
                          ts_in(request.confirmationDate, "confirmationDate"),
                          ts_in(request.requestedDate, "requestedDate"),
                          vf_start(request.validFor),
                          vf_end(request.validFor)});
    write_audit(txn, "RESERVE_BALANCE", reserve_id, "balance.reserve", json(request).dump());
    txn.commit();
    return {request, succeeded};
}

std::optional<bss_sid::ReserveBalance> BalanceStore::get_reserve(const std::string& id) {
    auto lease = pool_.acquire();
    pqxx::work txn(lease.conn());
    const auto result = txn.exec("SELECT " + kEventCols +
                                     ", reason FROM balance_mgmt.reserve_balance WHERE id = $1",
                                 pqxx::params{id});
    if (result.empty()) {
        return std::nullopt;
    }
    const auto r = result.front();
    bss_sid::ReserveBalance v{};
    fill_event_common(v, r);
    v.reason = s(r, "reason");
    return v;
}

} // namespace balance_management
