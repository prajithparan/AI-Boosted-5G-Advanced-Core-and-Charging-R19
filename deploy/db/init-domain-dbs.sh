#!/usr/bin/env bash
# ADR-0384: creates the consolidated domain databases on the postgres-chf instance and applies their
# DDL in file order -- `charging` (one schema per NF: party, product_catalog, subscriber_mgmt,
# balance_mgmt, chf_rating, roaming; deploy/db/charging/*.sql) and `orchestration` (the customer
# onboarding saga, deploy/db/orchestration/schema.sql).
#
# Runs as a postgres /docker-entrypoint-initdb.d/ script in compose (first start of an empty volume
# only), and is invoked the same way by CI against its service container. Idempotent on the database
# level (CREATE only if missing); the DDL files themselves assume a fresh database, except
# 31-product-lossless.sql which is re-runnable by design.
set -euo pipefail

DDL_ROOT="${DDL_ROOT:-/domain-ddl}"
PSQL=(psql -v ON_ERROR_STOP=1 -q --username "${POSTGRES_USER:-postgres}")

create_db() {
    local db="$1"
    if [ "$("${PSQL[@]}" -d postgres -Atc "SELECT 1 FROM pg_database WHERE datname = '${db}'")" != "1" ]; then
        "${PSQL[@]}" -d postgres -c "CREATE DATABASE ${db}"
    fi
}

create_db charging
for f in "${DDL_ROOT}"/charging/*.sql; do
    echo "init-domain-dbs: charging <- $(basename "$f")"
    "${PSQL[@]}" -d charging -f "$f"
done

create_db orchestration
echo "init-domain-dbs: orchestration <- schema.sql"
"${PSQL[@]}" -d orchestration -f "${DDL_ROOT}/orchestration/schema.sql"
