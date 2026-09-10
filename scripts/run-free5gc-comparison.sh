#!/usr/bin/env bash
# ADR-0329: this project's NRF NFDiscovery vs free5GC's, same load generator, matched config.
# Read docs/BENCHMARK_METHOD.md first -- it fixes the rules and was committed before any run.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build-release/free5gc-comparison}"
SP="${SCRATCH:?set SCRATCH to the dir holding f5gc.token/ours.token}"
mkdir -p "$OUT"
LG="$ROOT/build-release/tools/sbi-loadgen/sbi-loadgen"
QUERY="nf-instances?target-nf-type=UDM&requester-nf-type=AMF"
OURS="https://127.0.0.1:7777/nnrf-disc/v1/$QUERY"
# free5GC v3.4.4 REGENERATES its NRF certificate on every startup with SAN "DNS:NRF" only -- a
# supplied cert with IP:127.0.0.1 is overwritten, and a read-only cert mount makes it exit. So the
# client must connect by that name. HOSTALIASES maps it per-process through glibc, which needs no
# root and modifies nothing on the machine. The connection still goes to 127.0.0.1, so both
# systems are reached over the same loopback path.
export HOSTALIASES="$SP/hostaliases"
F5GC="https://NRF:18000/nnrf-disc/v1/$QUERY"
UDMID="12345678-1234-4123-8123-123456789abc"
AMFID="00000000-0000-4000-8000-0000000000aa"
REPEATS=3

{
  echo "date:        $(date -Is)"
  echo "kernel:      $(uname -sr)"
  echo "cpu:         $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "cores:       $(nproc)"
  echo "memory:      $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
  echo "commit:      $(cd "$ROOT" && git rev-parse --short HEAD)"
  echo "build type:  $(cmake -LA "$ROOT/build-release" 2>/dev/null | grep -m1 '^CMAKE_BUILD_TYPE' | cut -d= -f2)"
  echo "free5gc:     $(docker inspect --format '{{index .Config.Image}}' f5gc-nrf 2>/dev/null)"
  echo "also resident: $(ps -eo comm --sort=-rss | sed -n '2,4p' | tr '\n' ' ')"
  echo "note:        load generator and SUT share this host, loopback only, for BOTH systems"
} | tee "$OUT/environment.txt"

# Refuse to report a run that measured anything but 200s -- a benchmark that silently measured
# error responses is worse than no benchmark.
assert_200() {
  grep -qE '^status codes *: 200=' "$1" || { echo "REFUSING: $1 not an all-200 run" >&2; exit 1; }
  grep -qE '^status codes *:.*(401|403|404|500)=' "$1" && { echo "REFUSING: $1 has errors" >&2; exit 1; }
  return 0
}

# Each system is validated against its OWN CA. The first version passed this project's CA for
# both, so every free5GC request failed TLS verification and the run recorded 5865 transport
# errors and zero responses -- caught by assert_200 refusing to report it, which is what that
# guard is for.
ca_for() { [ "$1" = free5gc ] && echo "$SP/f5gc/cert/root.pem" || echo "$ROOT/certs/ca/ca.crt"; }

run() { # system url token label concurrency [extra...]
  local sys="$1" url="$2" tok="$3" label="$4" conc="$5"; shift 5
  local ca; ca="$(ca_for "$sys")"
  for r in $(seq 1 $REPEATS); do
    tok="$(token_for "$sys")"
    [ -n "$tok" ] || { echo "no token for $sys" >&2; exit 1; }
    local f="$OUT/$sys-$label-run$r.txt"
    "$LG" --url "$url" --cert "$ROOT/certs/hello-nf/cert.pem" --key "$ROOT/certs/hello-nf/key.pem" \
       --ca "$ca" --header "authorization: Bearer $tok" \
       --warmup 2 --duration 15 --concurrency "$conc" "$@" > "$f" 2>&1
    assert_200 "$f"
    printf "  %-8s %-12s run%d: %s\n" "$sys" "$label" "$r" "$(grep -m1 '^throughput' "$f" | sed 's/.*: //')"
  done
}

# A FRESH token per system per run. The baseline script learned this the hard way: a token
# fetched once drifted past its lifetime and a later run measured the 401 path at full speed.
# free5GC's tokens are shorter-lived than this project's, so it matters more here, not less.
ours_token() {
  curl -s --http2-prior-knowledge --cert "$ROOT/certs/hello-nf/cert.pem" \
    --key "$ROOT/certs/hello-nf/key.pem" --cacert "$ROOT/certs/ca/ca.crt" \
    -X POST "https://127.0.0.1:7777/oauth2/token" \
    -H "content-type: application/x-www-form-urlencoded" \
    -d "grant_type=client_credentials&nfInstanceId=bench&scope=nnrf-disc&targetNfType=NRF" \
    | python3 -c 'import sys,json;print(json.load(sys.stdin)["access_token"])'
}
f5gc_token() {
  curl -sk -X POST "https://127.0.0.1:18000/oauth2/token" \
    -H "content-type: application/x-www-form-urlencoded" \
    -d "grant_type=client_credentials&nfInstanceId=$AMFID&nfType=AMF&scope=nnrf-disc&targetNfType=NRF" \
    | python3 -c 'import sys,json;print(json.load(sys.stdin).get("access_token",""))'
}
token_for() { [ "$1" = free5gc ] && f5gc_token || ours_token; }

# free5GC drops every NF registration when its NRF restarts, and with oauth:true an unregistered
# requester cannot obtain a token -- so a re-run against a restarted container measures 401s at
# full speed rather than discovery. Re-registering is idempotent (PUT), so it is done every run
# instead of assumed. This is not tuning either system: it is putting the same two NF profiles in
# front of both NRFs, which the method requires.
ensure_free5gc_registered() {
  local udm="12345678-1234-4123-8123-123456789abc" amf="00000000-0000-4000-8000-0000000000aa"
  for pair in "$udm:$SP/udm-profile.json" "$amf:$SP/amf-profile.json"; do
    local id="${pair%%:*}" file="${pair#*:}"
    [ -f "$file" ] || { echo "missing NF profile $file" >&2; exit 1; }
    local code
    code=$(curl -sk -o /dev/null -w '%{http_code}' -X PUT \
      "https://127.0.0.1:18000/nnrf-nfm/v1/nf-instances/$id" \
      -H "content-type: application/json" --data-binary @"$file")
    case "$code" in 200|201) ;; *) echo "free5GC registration of $id failed: $code" >&2; exit 1;; esac
  done
}
ensure_free5gc_registered

for c in 1 8 32; do
  echo "== concurrency $c =="
  run ours  "$OURS" "" "c$c" "$c"
  run free5gc "$F5GC" "" "c$c" "$c"
done

echo "== open-loop 2000 rps, c32 =="
run ours    "$OURS" "" "open2000" 32 --rate 2000
run free5gc "$F5GC" "" "open2000" 32 --rate 2000

echo "results in $OUT"
