#!/usr/bin/env bash
# Run the charging/analytics pipeline end to end: drive the real CHF N40 path with cdr-traffic-gen
# to accumulate CDRs, which flow CHF -> (Kafka ->) Doris cdr table and, per feature-extract day,
# into chf_features.subscriber_features for NWDAF.
#
# Default target is the 2026-09-19 scale-up: 5,000,000 CDRs across 100,000 customers spanning both
# Consumer and Enterprise segments. Segmentation is intrinsic to the generator: profiles.cpp makes
# ~20% of the subscriber-index space Enterprise, the rest Consumer, so `--subscribers 100000` yields
# both segments with no extra flag.
#
# The generator drives the REAL charging engine over mTLS; CDRs are genuine charging output, only
# the offered load (who/when/how-much) is synthetic (ADR-0337). One session = Create+Update+Release
# = one CDR.
#
# Before launching the full run this does a measured warm-up (WARMUP_SESSIONS) and prints an ETA, so
# a multi-hour run is never launched blind. The full run is launched DETACHED (setsid+nohup) so it
# survives this shell, the session's memory reaper, and a context compaction; its PID and log path
# are printed. Poll the log with:  tail -f "$LOG"
set -euo pipefail
cd "$(dirname "$0")/.."

COMPOSE_FILE="${COMPOSE_FILE:-deploy/docker/docker-compose.yml}"
GEN="${GEN:-build/tools/cdr-traffic-gen/cdr-traffic-gen}"
CHF_URL="${CHF_URL:-https://127.0.0.1:7784}"
CERT="${CERT:-certs/chf/cert.pem}"
KEY="${KEY:-certs/chf/key.pem}"
CA="${CA:-certs/ca/ca.crt}"

SESSIONS="${SESSIONS:-5000000}"
SUBSCRIBERS="${SUBSCRIBERS:-100000}"
CONCURRENCY="${CONCURRENCY:-32}"
UPDATES="${UPDATES:-2}"          # usage-bearing Updates per session -- what makes a CDR trainable
WARMUP_SESSIONS="${WARMUP_SESSIONS:-10000}"
LOGDIR="${LOGDIR:-/tmp/claude-1000/-home-mastermind-5gc-r19/04f7cda0-f937-48ff-b78e-8b2bb7de33f1/scratchpad}"
mkdir -p "$LOGDIR"

[[ -x "$GEN" ]] || { echo "generator not built: $GEN  (build it: cmake --build build --target cdr-traffic-gen -j2)"; exit 1; }
for f in "$CERT" "$KEY" "$CA"; do [[ -r "$f" ]] || { echo "missing mTLS material: $f"; exit 1; }; done

# CHF must be reachable. The lab stack: bring it up first if it is not.
if ! (exec 3<>/dev/tcp/127.0.0.1/7784) 2>/dev/null; then
  echo "CHF not answering on 7784 -- bringing up the lab charging stack..."
  docker compose -f "$COMPOSE_FILE" up -d \
    valkey doris doris-schema-init postgres-chf kafka \
    nrf product-catalog balance-management chf
  echo "waiting for CHF..."
  for _ in $(seq 1 60); do (exec 3<>/dev/tcp/127.0.0.1/7784) 2>/dev/null && break; sleep 2; done
fi

run_gen() { "$GEN" --chf "$CHF_URL" --cert "$CERT" --key "$KEY" --ca "$CA" \
    --subscribers "$2" --sessions "$1" --concurrency "$CONCURRENCY" --updates "$UPDATES"; }

echo "== warm-up: $WARMUP_SESSIONS sessions over 1000 subscribers to measure throughput =="
start=$(date +%s)
run_gen "$WARMUP_SESSIONS" 1000
elapsed=$(( $(date +%s) - start )); elapsed=$(( elapsed > 0 ? elapsed : 1 ))
rate=$(( WARMUP_SESSIONS / elapsed ))
echo "  warm-up: $WARMUP_SESSIONS sessions in ${elapsed}s = ~${rate} CDR/s"
[[ $rate -gt 0 ]] && echo "  projected full run: $SESSIONS CDRs ~= $(( SESSIONS / rate / 60 )) min (~$(( SESSIONS / rate / 3600 )) h)"

echo
echo "== launching full run DETACHED: $SESSIONS CDRs over $SUBSCRIBERS customers (Consumer+Enterprise) =="
LOG="$LOGDIR/cdrgen-5M-$(date +%Y%m%d-%H%M%S).log"
setsid nohup "$GEN" --chf "$CHF_URL" --cert "$CERT" --key "$KEY" --ca "$CA" \
    --subscribers "$SUBSCRIBERS" --sessions "$SESSIONS" --concurrency "$CONCURRENCY" --updates "$UPDATES" \
    > "$LOG" 2>&1 &
PID=$!
echo "$PID" > "$LOG.pid"
echo "  PID $PID   log: $LOG"
echo "  follow:  tail -f \"$LOG\""
echo "  stop:    kill $PID"
