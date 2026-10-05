#!/usr/bin/env bash
# ADR-0446 (Create only) + ADR-0445's full baseline ask (Update, TMF654 balance ops, CPU/RSS,
# PgPool/PostgreSQL connections, extension added 2026-10-05): the committed-before-seeing-numbers
# baseline for CHF's charging data plane, across the increments ADR-0445 scoped (catalog snapshot,
# charging_data_ref correlator, PgPool hardening). Same discipline `docs/BENCHMARK_METHOD.md`
# already established for the NRF/free5GC comparison (ADR-0329): method fixed and committed first,
# result reported in whichever direction it lands.
#
# What this measures and what it does NOT:
#   * CHF's real Nchf_ConvergedCharging Create and Update paths, and balance-management's real
#     TMF654 reserveBalance/adjustBalance/topupBalance, end to end: NRF-registered CHF, a real
#     product-catalog (ProductOffering + ProductOfferingPrice, ratingGroup=10) and a real
#     balance-management bucket, over real HTTP/2 + mTLS, exactly the code path a real SMF drives.
#   * CPU/RSS (sampled from CHF's own PID) and PostgreSQL connection counts (charging DB,
#     before/after) alongside the existing chf_rating_db_pool_* Prometheus gauges.
#   * NOT measured, disclosed rather than silently skipped: Gy CCR-I/U/T (Diameter -- no HTTP
#     target for sbi-loadgen to drive), Nchf_ConvergedCharging Release, and multi-SUPI/
#     multi-bucket traffic (sbi-loadgen has no per-request body templating, so every case below is
#     single-SUPI/single-bucket, same limit as the original Create-only script).
#   * It is a BASELINE of this build on this machine, not a comparison against free5GC or anything
#     else (CHF has no free5GC equivalent in this project's own prior benchmarking -- ADR-0329 is
#     NRF-discovery only).
#   * The Create cases' own concurrency sweep (c1/c8/c32 closed-loop + open-loop) measures single-
#     SUPI, single-bucket CONTENTION (all requests serialize through one bucket's row lock), not
#     independent-subscriber traffic -- real and disclosed, not hidden. Update and the balance
#     operations are each measured at closed-loop c1 only, proportional to, not a second full
#     matrix duplicating, that sweep.
#   * Load generator and system under test share one host, over loopback -- inflates latency, caps
#     throughput, same caveat `run-baseline-benchmark.sh` already discloses.
#   * ADR-0009's synchronous HTTP client is still open throughout.
#
# Prerequisites this script does NOT bring up itself (bring them up first):
#   * Valkey/Redis on 127.0.0.1:6379 (CHF `std::terminate`s at startup without it -- COMPLIANCE_P1_
#     P15.md blocker 0a, a real known gap, not something this script should paper over).
#   * PostgreSQL reachable at the `charging` database on 127.0.0.1:5434 (postgres-chf in
#     deploy/docker/docker-compose.yml, or an equivalent local instance) with the charging schema
#     already applied.
#
# Usage: scripts/run-chf-rating-baseline.sh [output-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/benchmark-results-chf}"
mkdir -p "$OUT"

CERTS="$ROOT/certs"
NRF="$ROOT/build/nfs/nrf/nrf"
CATALOG="$ROOT/build/bss/product-catalog/product-catalog"
BALANCE="$ROOT/build/bss/balance-management/balance-management"
CHF="$ROOT/build/nfs/chf/chf"
LOADGEN="$ROOT/build/tools/sbi-loadgen/sbi-loadgen"

for bin in "$NRF" "$CATALOG" "$BALANCE" "$CHF" "$LOADGEN"; do
    [ -x "$bin" ] || { echo "missing $bin -- build first" >&2; exit 1; }
done

echo "== preflight: Valkey and PostgreSQL reachable =="
if ! (exec 3<>/dev/tcp/127.0.0.1/6379) 2>/dev/null; then
    echo "Valkey not reachable on 127.0.0.1:6379 -- bring it up first (CHF aborts without it)" >&2
    exit 1
fi
exec 3>&- 2>/dev/null || true
if ! (exec 3<>/dev/tcp/127.0.0.1/5434) 2>/dev/null; then
    echo "PostgreSQL not reachable on 127.0.0.1:5434 -- bring up postgres-chf first" >&2
    exit 1
fi
exec 3>&- 2>/dev/null || true

echo "== environment =="
{
    echo "date:    $(date -Is)"
    echo "kernel:  $(uname -sr)"
    echo "cpu:     $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cores:   $(nproc)"
    echo "memory:  $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
    echo "commit:  $(cd "$ROOT" && git rev-parse --short HEAD)"
    echo "note:    load generator and SUT share this host; all traffic over loopback"
    echo "note:    single-SUPI/single-bucket contention run, see this script's own header"
    echo "note:    CDR sink pointed at an unreachable Kafka broker (no Doris brought up) -- this"
    echo "         run measures rating/reservation only, not CDR persistence, see script header"
} | tee "$OUT/environment.txt"

PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT

"$NRF" >"$OUT/nrf.log" 2>&1 & PIDS+=($!)
sleep 2
"$CATALOG" >"$OUT/product-catalog.log" 2>&1 & PIDS+=($!)
"$BALANCE" >"$OUT/balance-management.log" 2>&1 & PIDS+=($!)
sleep 2
# Doris is CHF's default CDR sink (cdr_direct_insert=true) and this script deliberately does not
# bring up a Doris cluster just to benchmark the rating path -- CHF treats a disconnected Doris as
# FATAL by design (a real, correct architecture rule: a persistence failure must terminate, never
# degrade silently). Routed to a bogus event-bus broker instead: librdkafka's producer does not
# synchronously connect at construction (confirmed: `cdr_event_producer.cpp`'s constructor only
# builds a client config), so CHF starts fine and CDRs simply fail to deliver in the background --
# this run measures the rating/reservation path, not CDR persistence, and says so rather than
# silently disabling a real architecture guardrail.
CHF_CDR_DIRECT_INSERT=false CHF_CDR_EVENT_BUS_BROKERS=127.0.0.1:19999 \
    "$CHF" >"$OUT/chf.log" 2>&1 & CHF_PID=$!
PIDS+=("$CHF_PID")

wait_reachable() {
    local url="$1"
    for _ in $(seq 1 100); do
        if curl -s -o /dev/null --max-time 1 --cacert "$CERTS/ca/ca.crt" \
             --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" \
             -X POST "$url" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== waiting for services =="
CATALOG_BASE="https://127.0.0.1:7785/tmf-api/productCatalogManagement/v4"
BALANCE_BASE="https://127.0.0.1:7786/tmf-api/prepayBalanceManagement/v4"
CHF_URL="https://127.0.0.1:7784/nchf-convergedcharging/v3/chargingdata"
wait_reachable "$CATALOG_BASE/productOffering" || { echo "product-catalog never came up" >&2; exit 1; }
wait_reachable "$BALANCE_BASE/bucket" || { echo "balance-management never came up" >&2; exit 1; }
wait_reachable "$CHF_URL" || { echo "chf never came up" >&2; exit 1; }

echo "== provisioning a real ProductOfferingPrice/ProductOffering (ratingGroup=10) + balance =="
SUPI="imsi-999700000090001"

PRICE_JSON=$(curl -sf --cacert "$CERTS/ca/ca.crt" \
    --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" \
    -X POST "$CATALOG_BASE/productOfferingPrice" \
    -H "content-type: application/json" \
    -d '{
          "name": "CHF Rating Baseline Price",
          "priceType": "usage",
          "lifecycleStatus": "Active",
          "unitOfMeasure": {"amount": 10.0, "units": "GB"},
          "price": {"unit": "EUR", "value": 10.0},
          "prodSpecCharValueUse": [
            {"id": "rg-bench", "name": "ratingGroup",
             "productSpecCharacteristicValue": [{"value": 10}]}
          ]
        }')
PRICE_ID=$(echo "$PRICE_JSON" | python3 -c 'import sys,json; print(json.load(sys.stdin)["id"])')
echo "price id: $PRICE_ID"

OFFERING_ID=$(curl -sf --cacert "$CERTS/ca/ca.crt" \
    --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" \
    -X POST "$CATALOG_BASE/productOffering" \
    -H "content-type: application/json" \
    -d "{\"name\": \"CHF Rating Baseline Offering\", \"lifecycleStatus\": \"Active\",
         \"isSellable\": true,
         \"productOfferingPrice\": [{\"id\": \"$PRICE_ID\"}]}" \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["id"])')
echo "offering created: $OFFERING_ID"

# Large headroom: every Create in this run reserves against the same bucket with no matching
# Release, so thousands of requests must not run the bucket dry mid-benchmark and start measuring
# the insufficient-funds rejection path instead of real rating.
curl -sf --cacert "$CERTS/ca/ca.crt" \
    --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" \
    -X POST "$BALANCE_BASE/topupBalance" \
    -H "content-type: application/json" \
    -d "{\"amount\": {\"amount\": 100000000, \"units\": \"monetary\"},
         \"bucket\": {\"id\": \"$SUPI\"}}" >/dev/null
echo "balance topped up for $SUPI"

BODY=$(python3 -c "
import json, datetime
print(json.dumps({
    'nfConsumerIdentification': {'nodeFunctionality': 'SMF'},
    'invocationTimeStamp': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000Z'),
    'invocationSequenceNumber': 1,
    'subscriberIdentifier': '$SUPI',
    'multipleUnitUsage': [{'ratingGroup': 10}],
}))
")

assert_all_status() {
    local expect="$1" file="$2"
    if ! grep -qE "^status codes *: ${expect}=" "$file" || \
       grep -qE '^status codes *:.*(400|401|403|404|500)=' "$file"; then
        echo "REFUSING TO REPORT: $file did not record an all-${expect} run:" >&2
        grep -E '^status codes' "$file" >&2
        exit 1
    fi
}
assert_all_201() { assert_all_status 201 "$1"; }

# CPU/RSS: ADR-0445's baseline ask, added here rather than a separate tool -- `ps` sampled once
# per second against CHF's own PID for the duration of each case, min/mean/max over the samples.
# Real resource usage of the process actually serving the run, not a synthetic cgroup figure.
# Each tick is appended to the .raw file immediately (not accumulated in memory and written once
# at loop exit) -- this sampler is killed from outside, not left to exit its own loop, so a
# write-at-the-end design would never produce a file at all.
sample_resources() {
    local rawfile="$1"
    : > "$rawfile"
    # Per tick: RSS (kB) and cumulative utime+stime clock ticks from /proc/<pid>/stat. `ps %cpu`
    # is NOT used: it is the lifetime average (cputime / elapsed since start), so it smears every
    # earlier case into this one -- the first run of this sampler reported exactly that artefact.
    while [ -r "/proc/$CHF_PID/stat" ]; do
        local rss ticks
        rss=$(awk '/^VmRSS:/{print $2}' "/proc/$CHF_PID/status" 2>/dev/null) || break
        ticks=$(awk '{print $14 + $15}' "/proc/$CHF_PID/stat" 2>/dev/null) || break
        echo "$(date +%s.%N) $rss $ticks" >> "$rawfile"
        sleep 1
    done
}

summarize_resources() {
    local rawfile="$1" outfile="$2"
    python3 -c "
import os
hz = os.sysconf('SC_CLK_TCK')
rows = []
with open('$rawfile') as f:
    for line in f:
        parts = line.split()
        if len(parts) == 3:
            rows.append((float(parts[0]), int(parts[1]), int(parts[2])))
rss = [r[1] for r in rows]
# % of ONE core over each 1 s interval (can exceed 100 on a multi-threaded process).
cpu = [100.0 * (b[2] - a[2]) / hz / (b[0] - a[0]) for a, b in zip(rows, rows[1:]) if b[0] > a[0]]
if rss and cpu:
    print(f'rss_kb min={min(rss)} mean={sum(rss)//len(rss)} max={max(rss)} (n={len(rss)} samples)')
    print(f'cpu_pct_of_one_core min={min(cpu):.1f} mean={sum(cpu)/len(cpu):.1f} max={max(cpu):.1f}')
else:
    print('no samples captured (case shorter than the 1s sample interval)')
" > "$outfile" 2>&1 || echo "resource summary failed" > "$outfile"
}

run_case() {
    local label="$1"; shift
    echo "== $label ($(date +%H:%M:%S)) =="
    sample_resources "$OUT/$label.resources.raw" &
    local sampler_pid=$!
    "$LOADGEN" --url "$CHF_URL" --method POST \
        --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" --ca "$CERTS/ca/ca.crt" \
        --header "content-type: application/json" --body "$BODY" \
        --warmup 2 --duration 15 "$@" --json "$OUT/$label.json" | tee "$OUT/$label.txt"
    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    assert_all_201 "$OUT/$label.txt"
    summarize_resources "$OUT/$label.resources.raw" "$OUT/$label.resources.txt"
    echo "-- resources during $label --"
    cat "$OUT/$label.resources.txt" || true
}

pg_connection_count() {
    docker exec docker-postgres-chf-1 psql -U postgres -d charging -tA \
        -c "select count(*) from pg_stat_activity;" 2>/dev/null || echo "unavailable (docker exec failed)"
}

echo "== PostgreSQL connections (charging DB) before the run: $(pg_connection_count) =="

for c in 1 8 32; do
    run_case "closed-c$c" --concurrency "$c"
done
run_case "open-200rps" --concurrency 32 --rate 200

# ADR-0445's baseline ask named Update and the three TMF654 balance operations too, not Create
# alone. One closed-c1 case each -- proportional to Create's own already-covered concurrency
# sweep, not a second full matrix. Disclosed, not silently out of scope: Gy CCR-I/U/T (Diameter,
# no HTTP target for sbi-loadgen to drive), Release, and multi-SUPI contention are NOT measured
# here -- sbi-loadgen has no per-request body templating (same disclosed limit as Create's own
# single-SUPI scope above), so a multi-subscriber run needs a different tool, not attempted in
# this pass.
run_single_case() {
    local label="$1" method="$2" url="$3" body="$4" expect_status="$5"
    echo "== $label ($(date +%H:%M:%S)) =="
    sample_resources "$OUT/$label.resources.raw" &
    local sampler_pid=$!
    "$LOADGEN" --url "$url" --method "$method" \
        --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" --ca "$CERTS/ca/ca.crt" \
        --header "content-type: application/json" --body "$body" \
        --warmup 2 --duration 15 --concurrency 1 --json "$OUT/$label.json" | tee "$OUT/$label.txt"
    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    assert_all_status "$expect_status" "$OUT/$label.txt"
    summarize_resources "$OUT/$label.resources.raw" "$OUT/$label.resources.txt"
    echo "-- resources during $label --"
    cat "$OUT/$label.resources.txt" || true
}

# A real ChargingDataRef to Update against -- not the load-test's own refs (those are all
# Create's, never Updated), a dedicated one from a single real Create call.
UPDATE_REF_LOCATION=$(curl -sfi --cacert "$CERTS/ca/ca.crt" \
    --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" \
    -X POST "$CHF_URL" -H "content-type: application/json" -d "$BODY" \
    | grep -i '^location:' | tr -d '\r' | awk '{print $2}')
UPDATE_URL="https://127.0.0.1:7784${UPDATE_REF_LOCATION}/update"
echo "update target: $UPDATE_URL"
run_single_case "update-c1" POST "$UPDATE_URL" "$BODY" 200

run_single_case "balance-reserve-c1" POST "$BALANCE_BASE/reserveBalance" \
    "{\"amount\": {\"amount\": 1, \"units\": \"monetary\"}, \"bucket\": {\"id\": \"$SUPI\"}}" 201
run_single_case "balance-adjust-c1" POST "$BALANCE_BASE/adjustBalance" \
    "{\"amount\": {\"amount\": 1, \"units\": \"monetary\"}, \"bucket\": {\"id\": \"$SUPI\"}}" 201
run_single_case "balance-topup-c1" POST "$BALANCE_BASE/topupBalance" \
    "{\"amount\": {\"amount\": 1, \"units\": \"monetary\"}, \"bucket\": {\"id\": \"$SUPI\"}}" 201

echo "== PostgreSQL connections (charging DB) after the run: $(pg_connection_count) =="

# Remove this run's own catalog entries (real TMF620 DELETE) so they cannot outlive the benchmark.
# Every earlier run left an Active ratingGroup=10 offering behind in the shared lab catalog, and
# CHF rates against the FIRST matching Active price in collection order -- so the leftovers were
# silently capturing every later ratingGroup=10 request (the CDR generator's music content class).
for path in "productOffering/$OFFERING_ID" "productOfferingPrice/$PRICE_ID"; do
    curl -s -o /dev/null -w "  DELETE $path -> %{http_code}\n" --cacert "$CERTS/ca/ca.crt" \
        --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" \
        -X DELETE "$CATALOG_BASE/$path" || true
done

echo "== CHF's own metrics after the run =="
curl -s http://127.0.0.1:9472/metrics 2>/dev/null | grep -E '^chf_' | tee "$OUT/chf-counters.txt" || true
echo "== PgPool-specific metrics (ADR-0449/0450) =="
grep -E '^chf_rating_db_pool' "$OUT/chf-counters.txt" | tee "$OUT/pgpool-counters.txt" || true

echo
echo "results written to $OUT"
