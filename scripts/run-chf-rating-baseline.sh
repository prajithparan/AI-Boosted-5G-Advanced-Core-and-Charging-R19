#!/usr/bin/env bash
# ADR-0446: the committed-before-seeing-numbers baseline for CHF's CURRENT Nchf_ConvergedCharging_
# Create rating path (the N+1 ProductOffering/ProductOfferingPrice SBI lookups ADR-0445 found),
# taken before the in-memory catalog/policy snapshot that same ADR scopes is built. Same discipline
# `docs/BENCHMARK_METHOD.md` already established for the NRF/free5GC comparison (ADR-0329): method
# fixed and committed first, result reported in whichever direction it lands.
#
# What this measures and what it does NOT:
#   * CHF's real Nchf_ConvergedCharging_Create path, end to end: NRF-registered CHF, a real
#     product-catalog (ProductOffering + ProductOfferingPrice, ratingGroup=10) and a real
#     balance-management bucket, over real HTTP/2 + mTLS, exactly the code path a real SMF drives.
#   * It is a BASELINE of this build on this machine, not a comparison against free5GC or anything
#     else (CHF has no free5GC equivalent in this project's own prior benchmarking -- ADR-0329 is
#     NRF-discovery only).
#   * Every request in a run is the SAME ChargingDataRequest (same SUPI, same ratingGroup) --
#     sbi-loadgen has no per-request body templating. This measures single-SUPI, single-bucket
#     CONTENTION (all requests serialize through one bucket's row lock), not independent-subscriber
#     traffic. Real and disclosed, not hidden: a production workload with many distinct subscribers
#     would not contend on one row the way this run does.
#   * Load generator and system under test share one host, over loopback -- inflates latency, caps
#     throughput, same caveat `run-baseline-benchmark.sh` already discloses.
#   * ADR-0009's synchronous HTTP client is still open, and so is the N+1 catalog-read path this
#     baseline exists to characterise -- both deliberately, so this number is the "before".
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
    "$CHF" >"$OUT/chf.log" 2>&1 & PIDS+=($!)

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

curl -sf --cacert "$CERTS/ca/ca.crt" \
    --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" \
    -X POST "$CATALOG_BASE/productOffering" \
    -H "content-type: application/json" \
    -d "{\"name\": \"CHF Rating Baseline Offering\", \"lifecycleStatus\": \"Active\",
         \"isSellable\": true,
         \"productOfferingPrice\": [{\"id\": \"$PRICE_ID\"}]}" >/dev/null
echo "offering created"

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

assert_all_201() {
    local file="$1"
    if ! grep -qE '^status codes *: 201=' "$file" || grep -qE '^status codes *:.*(400|401|403|404|500)=' "$file"; then
        echo "REFUSING TO REPORT: $file did not record an all-201 run:" >&2
        grep -E '^status codes' "$file" >&2
        exit 1
    fi
}

run_case() {
    local label="$1"; shift
    echo "== $label ($(date +%H:%M:%S)) =="
    "$LOADGEN" --url "$CHF_URL" --method POST \
        --cert "$CERTS/hello-nf/cert.pem" --key "$CERTS/hello-nf/key.pem" --ca "$CERTS/ca/ca.crt" \
        --header "content-type: application/json" --body "$BODY" \
        --warmup 2 --duration 15 "$@" --json "$OUT/$label.json" | tee "$OUT/$label.txt"
    assert_all_201 "$OUT/$label.txt"
}

for c in 1 8 32; do
    run_case "closed-c$c" --concurrency "$c"
done
run_case "open-200rps" --concurrency 32 --rate 200

echo "== CHF's own metrics after the run =="
curl -s http://127.0.0.1:9472/metrics 2>/dev/null | grep -E '^chf_' | tee "$OUT/chf-counters.txt" || true

echo
echo "results written to $OUT"
