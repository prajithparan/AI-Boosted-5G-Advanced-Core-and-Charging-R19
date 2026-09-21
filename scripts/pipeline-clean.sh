#!/usr/bin/env bash
# Clean the generated charging/analytics corpus so the pipeline can be re-run from empty.
#
# Scope (verified against README.md line 222 and the schema files, 2026-09-19): the generated data
# is the CDR corpus and everything DERIVED from it -- NOT the seeded reference/config data. The
# "75,000 subscribers" of the 3M run is the distinct-SUPI span of `--subscribers`, not rows in a
# BSS subscriber table (there is no subscriber-provisioning path -- README, ADR-0025/0330). So this
# truncates:
#   * Doris chf_cdr.cdr                    -- the CDRs themselves
#   * Doris chf_features.subscriber_features -- the NWDAF feature store extracted from them
#   * Redis (valkey) chf:* keys            -- ChargingDataRef counters + per-ref content hashes
#   * postgres chf_rating.rating_decision  -- CHF's own TMF678 rating decisions
#   * postgres chf_rating.audit_record     -- CHF audit trail for those decisions
# and DELIBERATELY LEAVES seeded config untouched: product-catalog (product_offering/_price/
# _specification), roaming-interconnect agreements, balance buckets funded out-of-band. Truncating
# those would break rating on the next run.
#
# Note (state it, do not hide it): commit 743acdc records the 3M corpus as the training data the
# CHF models were fitted on. Truncating it invalidates the data lineage of the existing MLflow runs
# for those models. Re-training against the fresh 5M corpus is the intended follow-up.
#
# Destructive. Does nothing without --yes. Prints row counts before and after so the operator sees
# exactly what was removed.
#
# Usage:
#   scripts/pipeline-clean.sh            # dry run: show current counts, change nothing
#   scripts/pipeline-clean.sh --yes      # actually truncate/reset
#
# Runs every statement THROUGH the compose services (doris/valkey/postgres-chf reach each other by
# hostname on the compose network), so the lab stack must be up first:
#   docker compose -f deploy/docker/docker-compose.yml up -d valkey doris doris-schema-init postgres-chf
set -euo pipefail

COMPOSE_FILE="${COMPOSE_FILE:-deploy/docker/docker-compose.yml}"
DC=(docker compose -f "$COMPOSE_FILE")

APPLY=0
[[ "${1:-}" == "--yes" ]] && APPLY=1

# The Doris allinone container has a mysql client; exec into the running service (a `compose run`
# on doris-schema-init swallows an ad-hoc command via its entrypoint, so exec the live doris).
doris() { "${DC[@]}" exec -T doris mysql -h127.0.0.1 -P9030 -uroot -N -B -e "$1"; }
redis() { "${DC[@]}" exec -T valkey redis-cli "$@"; }
pg()    { "${DC[@]}" exec -T postgres-chf psql -U postgres -d chf_rating -At -c "$1"; }

echo "== current corpus size =="
echo "  doris chf_cdr.cdr rows:                    $(doris 'SELECT COUNT(*) FROM chf_cdr.cdr'                          2>/dev/null || echo '?')"
echo "  doris chf_features.subscriber_features:    $(doris 'SELECT COUNT(*) FROM chf_features.subscriber_features'    2>/dev/null || echo '?')"
echo "  redis chf:* keys:                          $(redis --scan --pattern 'chf:*' 2>/dev/null | wc -l | tr -d ' ')"
echo "  pg chf_rating.rating_decision rows:        $(pg 'SELECT COUNT(*) FROM rating_decision'                        2>/dev/null || echo '?')"
echo "  pg chf_rating.audit_record rows:           $(pg 'SELECT COUNT(*) FROM audit_record'                          2>/dev/null || echo '?')"

if [[ $APPLY -eq 0 ]]; then
  echo
  echo "dry run -- nothing changed. Re-run with --yes to truncate the above."
  exit 0
fi

echo
echo "== truncating =="
# Doris: TRUNCATE, never DELETE -- a multi-million-row DELETE on a UNIQUE KEY table is pathological
# (schema.doris.sql line 80 records exactly this lesson for retention).
doris 'TRUNCATE TABLE chf_cdr.cdr'                       && echo "  truncated chf_cdr.cdr"
doris 'TRUNCATE TABLE chf_features.subscriber_features'  && echo "  truncated chf_features.subscriber_features"

# Redis: drop every chf:* key (content hashes + the next_id counters), so new CDRs start from ref 1
# and cannot collide with an archived ref. --scan avoids KEYS on a large keyspace.
n=$(redis --scan --pattern 'chf:*' | wc -l | tr -d ' ')
redis --scan --pattern 'chf:*' | while read -r k; do [[ -n "$k" ]] && redis DEL "$k" >/dev/null; done
echo "  deleted $n redis chf:* keys"

# CHF rating store.
pg 'TRUNCATE TABLE rating_decision' && echo "  truncated rating_decision"
pg 'TRUNCATE TABLE audit_record'    && echo "  truncated audit_record"

echo
echo "== after =="
echo "  doris chf_cdr.cdr rows:                    $(doris 'SELECT COUNT(*) FROM chf_cdr.cdr' 2>/dev/null || echo '?')"
echo "  redis chf:* keys:                          $(redis --scan --pattern 'chf:*' 2>/dev/null | wc -l | tr -d ' ')"
echo "clean complete."
