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

# Warm-up MUST use the same --subscribers as the full run: profile_for(idx, total_subscribers)
# derives segment/product/volume from BOTH the index AND the total, so a SUPI drawn at total=1000
# would get a different (contradictory) profile than the same SUPI at total=100000. Same total ->
# the warm-up's CDRs are consistent members of the final corpus (refs stay unique via Redis INCR),
# so they need not be cleaned out afterwards.
echo "== warm-up: $WARMUP_SESSIONS sessions over $SUBSCRIBERS subscribers to measure throughput =="
start=$(date +%s)
run_gen "$WARMUP_SESSIONS" "$SUBSCRIBERS"
elapsed=$(( $(date +%s) - start )); elapsed=$(( elapsed > 0 ? elapsed : 1 ))
rate=$(( WARMUP_SESSIONS / elapsed ))
echo "  warm-up: $WARMUP_SESSIONS sessions in ${elapsed}s = ~${rate} CDR/s"
if [[ $rate -gt 0 ]]; then
  remaining=$(( SESSIONS - WARMUP_SESSIONS ))
  echo "  projected remaining run: $remaining CDRs ~= $(( remaining / rate / 60 )) min (~$(( remaining / rate / 3600 )) h)"
fi

# The full run generates the REMAINING CDRs (the warm-up's rows already count toward the target).
REMAINING=$(( SESSIONS - WARMUP_SESSIONS )); [[ $REMAINING -lt 0 ]] && REMAINING=0

# The COMPLETE pipeline is CDR gen -> Doris -> feature store -> NWDAF. The detached job below runs
# the generator and THEN, on success, extracts the NWDAF feature store for every date the run
# spanned (extract_features.py prints per-date SQL; pipe it into Doris). Without this stage
# chf_features.subscriber_features stays empty and the NWDAF half of the scale-up does not exist.
echo
echo "== launching full pipeline DETACHED: $REMAINING more CDRs over $SUBSCRIBERS customers, then feature extract =="
LOG="$LOGDIR/pipeline-5M-$(date +%Y%m%d-%H%M%S).log"
STAGE="$LOGDIR/pipeline-stage-$(date +%Y%m%d-%H%M%S).sh"
cat > "$STAGE" <<STAGEEOF
#!/usr/bin/env bash
set -uo pipefail
cd "$(pwd)"
start_date=\$(date +%F)
echo "[\$(date -Is)] generator: $REMAINING sessions over $SUBSCRIBERS subscribers"
"$GEN" --chf "$CHF_URL" --cert "$CERT" --key "$KEY" --ca "$CA" \
    --subscribers "$SUBSCRIBERS" --sessions "$REMAINING" --concurrency "$CONCURRENCY" --updates "$UPDATES"
gen_rc=\$?
echo "[\$(date -Is)] generator exited rc=\$gen_rc"
[[ \$gen_rc -ne 0 ]] && { echo "generator failed -- skipping feature extract"; exit \$gen_rc; }
end_date=\$(date +%F)
echo "[\$(date -Is)] feature extract for dates \$start_date .. \$end_date"
d="\$start_date"
while :; do
  echo "[\$(date -Is)] extract_features \$d"
  python3 tools/chf-features/extract_features.py --date "\$d" \
    | docker compose -f "$COMPOSE_FILE" exec -T doris mysql -h127.0.0.1 -P9030 -uroot chf_features
  [[ "\$d" == "\$end_date" ]] && break
  d=\$(date -I -d "\$d + 1 day")
done
echo "[\$(date -Is)] pipeline complete"
STAGEEOF
chmod +x "$STAGE"
setsid nohup bash "$STAGE" > "$LOG" 2>&1 &
PID=$!
echo "$PID" > "$LOG.pid"
echo "  PID $PID   log: $LOG   stage script: $STAGE"
echo "  follow:  tail -f \"$LOG\""
echo "  stop:    kill $PID"
