#!/usr/bin/env bash
# Run the charging/analytics pipeline end to end: drive the real CHF N40 path with cdr-traffic-gen
# to accumulate CDRs, which flow CHF -> (Kafka ->) Doris cdr table and, per feature-extract day,
# into chf_features.subscriber_features for NWDAF.
#
# Default target is the 2026-09-19 scale-up: 5,000,000 CDR ROWS across 100,000 customers spanning
# both Consumer and Enterprise segments (ADR-0455: "5M CDRs" is counted in the same unit as the old
# "3M" corpus, i.e. Doris rows -- user-confirmed 2026-10-05). Segmentation is intrinsic to the
# generator: profiles.cpp makes ~20% of the subscriber-index space Enterprise, the rest Consumer, so
# `--subscribers 100000` yields both segments with no extra flag.
#
# The generator drives the REAL charging engine over mTLS; CDRs are genuine charging output, only
# the offered load (who/when/how-much) is synthetic (ADR-0337). One session writes (UPDATES + 2) CDR
# rows -- Create, each Update, and Release -- so the default 1,250,000 sessions x 4 = 5,000,000.
# (This header used to say "one session = one CDR"; the 2026-09 corpus shows exactly 4.0 rows per
# session, 3,334,649 rows over 833,672 refs.)
#
# Run scripts/pipeline-seed.sh first, or every CDR carries zero grant and zero cost (ADR-0455).
#
# Before launching the full run this does a measured warm-up (WARMUP_SESSIONS) and prints an ETA, so
# a multi-hour run is never launched blind. The full run is launched DETACHED (setsid+nohup) so it
# survives this shell, the session's memory reaper, and a context compaction; its PID and log path
# are printed. Poll the log with:  tail -f "$LOG"
set -euo pipefail
cd "$(dirname "$0")/.."

COMPOSE_FILE="${COMPOSE_FILE:-deploy/docker/docker-compose.yml}"
GEN="${GEN:-build-release/tools/cdr-traffic-gen/cdr-traffic-gen}"
CHF_URL="${CHF_URL:-https://127.0.0.1:7784}"
# ADR-0339: several CHF processes add write capacity (each CdrWriter serializes on its own
# connection). Space-separated; defaults to the single CHF_URL.
CHF_URLS="${CHF_URLS:-$CHF_URL}"
CHF_ARGS=""; for u in $CHF_URLS; do CHF_ARGS="$CHF_ARGS --chf $u"; done
CERT="${CERT:-certs/chf/cert.pem}"
KEY="${KEY:-certs/chf/key.pem}"
CA="${CA:-certs/ca/ca.crt}"

SESSIONS="${SESSIONS:-1250000}"
SUBSCRIBERS="${SUBSCRIBERS:-100000}"
CONCURRENCY="${CONCURRENCY:-32}"
UPDATES="${UPDATES:-2}"          # usage-bearing Updates per session -- what makes a CDR trainable
WARMUP_SESSIONS="${WARMUP_SESSIONS:-10000}"
LOGDIR="${LOGDIR:-$PWD/build-release/pipeline-logs}"
mkdir -p "$LOGDIR"

[[ -x "$GEN" ]] || { echo "generator not built: $GEN  (build it: cmake --build build-release --target cdr-traffic-gen -j2)"; exit 1; }
for f in "$CERT" "$KEY" "$CA"; do [[ -r "$f" ]] || { echo "missing mTLS material: $f"; exit 1; }; done

# CHF must already be reachable, from CURRENT host binaries. This used to `docker compose up` the
# lab NF images when CHF was down -- the exact path that once silently dropped every CDR (stale
# images, see project memory "pipeline stale images"). Refuse instead of guessing.
CHF_PORT="${CHF_URL##*:}"
if ! (exec 3<>/dev/tcp/127.0.0.1/"$CHF_PORT") 2>/dev/null; then
  echo "CHF not answering on $CHF_PORT. Start current HOST binaries first (build-release/: nrf," >&2
  echo "product-catalog, balance-management, chf) against the dockerized datastores." >&2
  exit 1
fi

doris_rows() {
  docker compose -f "$COMPOSE_FILE" exec -T doris mysql -h127.0.0.1 -P9030 -uroot -N -B \
    -e "SELECT COUNT(*) FROM chf_cdr.cdr"
}

run_gen() { "$GEN" $CHF_ARGS --cert "$CERT" --key "$KEY" --ca "$CA" \
    --subscribers "$2" --sessions "$1" --concurrency "$CONCURRENCY" --updates "$UPDATES"; }

# Warm-up MUST use the same --subscribers as the full run: profile_for(idx, total_subscribers)
# derives segment/product/volume from BOTH the index AND the total, so a SUPI drawn at total=1000
# would get a different (contradictory) profile than the same SUPI at total=100000. Same total ->
# the warm-up's CDRs are consistent members of the final corpus (refs stay unique via Redis INCR),
# so they need not be cleaned out afterwards.
echo "== warm-up: $WARMUP_SESSIONS sessions over $SUBSCRIBERS subscribers to measure throughput =="
rows_before=$(doris_rows)
start=$(date +%s)
run_gen "$WARMUP_SESSIONS" "$SUBSCRIBERS"
elapsed=$(( $(date +%s) - start )); elapsed=$(( elapsed > 0 ? elapsed : 1 ))

# Consistency probe (user rule, 2026-09-20: never bulk-load until created == stored is proven).
# Every warm-up session must have landed exactly (UPDATES + 2) rows. Waits out CHF's batched
# flush before counting -- must exceed 2x cdr_flush_interval_ms (ADR-0458's flusher wakes once
# per interval), hence the override.
sleep "${PROBE_SETTLE_SECONDS:-15}"
expected=$(( WARMUP_SESSIONS * (UPDATES + 2) ))
stored=$(( $(doris_rows) - rows_before ))
echo "  consistency probe: expected $expected new CDR rows, Doris holds $stored"
if [[ $stored -ne $expected ]]; then
  echo "REFUSING the full run: created != stored. Investigate before loading at scale." >&2
  exit 1
fi
rate=$(( WARMUP_SESSIONS / elapsed ))
echo "  warm-up: $WARMUP_SESSIONS sessions in ${elapsed}s = ~${rate} sessions/s"
if [[ $rate -gt 0 ]]; then
  remaining=$(( SESSIONS - WARMUP_SESSIONS ))
  echo "  projected remaining run: $remaining sessions ~= $(( remaining / rate / 60 )) min (~$(( remaining / rate / 3600 )) h)"
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
"$GEN" $CHF_ARGS --cert "$CERT" --key "$KEY" --ca "$CA" \
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
