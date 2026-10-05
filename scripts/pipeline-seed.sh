#!/usr/bin/env bash
# ADR-0455: seed the reference data the charging pipeline needs to RATE, before pipeline-run.sh.
#
# Why this exists: the 2026-09 pipeline corpus (3.3M CDR rows) carried ZERO granted units and ZERO
# cost -- the catalog had no ProductOfferingPrice for the generator's rating groups and no balance
# bucket existed for its subscribers, so CHF answered every session "granting nothing". This seeds
# both, through the real APIs where they exist:
#   1. one LAB ProductOfferingPrice + ProductOffering per rating group the generator emits (1-11,
#      tools/cdr-traffic-gen/src/profiles.cpp), via the real TMF620 POSTs;
#   2. one funded bucket per distinct bucket key over --subscribers, via the real TMF654
#      topupBalance (cdr-traffic-gen --fund-buckets, which derives keys from profile_for itself);
#   3. shared-bucket membership (family households, enterprise cost centres) by direct SQL into
#      balance_mgmt.bucket_related_party + is_shared -- TMF654 as built here has no membership
#      operation; test_balance_shared_bucket.cpp seeds it the same way.
#
# The tariff is LAB DATA, user-approved 2026-10-05 ("seed a lab tariff"), not a commercial rate
# card: every name carries "LAB" and ADR-0455, so it cannot be mistaken for one. Rates are
# deliberately round and plausible; they exist so CDRs carry real grants and real cost.
#
# Prerequisites: product-catalog (7785) and balance-management (7786) running from HOST binaries
# against postgres-chf (5434), host certs/. Idempotent for buckets (topup credits again) but NOT
# for the catalog: re-running adds a second LAB offering set -- run pipeline-clean.sh's catalog
# note first if re-seeding.
#
# Usage: SUBSCRIBERS=100000 scripts/pipeline-seed.sh
set -euo pipefail
cd "$(dirname "$0")/.."

GEN="${GEN:-build-release/tools/cdr-traffic-gen/cdr-traffic-gen}"
SUBSCRIBERS="${SUBSCRIBERS:-100000}"
FUND_AMOUNT="${FUND_AMOUNT:-100000}"
CATALOG="${CATALOG_BASE:-https://127.0.0.1:7785}/tmf-api/productCatalogManagement/v4"
BALANCE="${BALANCE_BASE:-https://127.0.0.1:7786}"
CURL=(curl -sf --cacert certs/ca/ca.crt --cert certs/hello-nf/cert.pem --key certs/hello-nf/key.pem
      -H "content-type: application/json")
pg() { docker exec -i docker-postgres-chf-1 psql -U postgres -d charging -v ON_ERROR_STOP=1 "$@"; }

[[ -x "$GEN" ]] || { echo "generator not built: $GEN" >&2; exit 1; }

# rating_group | name | unitOfMeasure amount | units | price EUR
TARIFF=(
  "1|Data|100|MB|1.00"
  "2|Voice (time)|5|MIN|0.10"
  "3|Events|10|unit|0.05"
  "4|Enterprise slice data|1|GB|5.00"
  "5|SMS|1|unit|0.05"
  "6|MMS|1|unit|0.20"
  "7|Voice step|1|MIN|0.02"
  "8|Content video|500|MB|2.00"
  "9|Content social|200|MB|0.50"
  "10|Content music|200|MB|0.50"
  "11|Fair-use throttled tier|100|MB|0.50"
)

ONLY_MEMBERSHIP="${ONLY_MEMBERSHIP:-0}" # 1 = skip steps 1-2 (recovery after a failed step 3)
echo "== 1. LAB tariff: one TMF620 price + offering per rating group =="
[[ "$ONLY_MEMBERSHIP" == 1 ]] && TARIFF=() && echo "  skipped (ONLY_MEMBERSHIP=1)"
for row in "${TARIFF[@]}"; do
  IFS='|' read -r rg name amount units price <<<"$row"
  price_id=$("${CURL[@]}" -X POST "$CATALOG/productOfferingPrice" -d "{
      \"name\": \"LAB pipeline tariff RG$rg $name (ADR-0455)\",
      \"priceType\": \"usage\", \"lifecycleStatus\": \"Active\",
      \"unitOfMeasure\": {\"amount\": $amount, \"units\": \"$units\"},
      \"price\": {\"unit\": \"EUR\", \"value\": $price},
      \"prodSpecCharValueUse\": [{\"id\": \"rg-lab-$rg\", \"name\": \"ratingGroup\",
                                 \"productSpecCharacteristicValue\": [{\"value\": $rg}]}]
    }" | python3 -c 'import sys,json; print(json.load(sys.stdin)["id"])')
  offering_id=$("${CURL[@]}" -X POST "$CATALOG/productOffering" -d "{
      \"name\": \"LAB pipeline offering RG$rg $name (ADR-0455)\",
      \"lifecycleStatus\": \"Active\", \"isSellable\": true,
      \"productOfferingPrice\": [{\"id\": \"$price_id\"}]
    }" | python3 -c 'import sys,json; print(json.load(sys.stdin)["id"])')
  printf '  RG%-3s %-26s %5s %-4s = %5s EUR   price %s offering %s\n' \
    "$rg" "$name" "$amount" "$units" "$price" "$price_id" "$offering_id"
done

echo "== 2. funding one bucket per bucket key over $SUBSCRIBERS subscribers =="
MEMBERS="$(mktemp)"
trap 'rm -f "$MEMBERS"' EXIT
EXTRA=(); [[ "$ONLY_MEMBERSHIP" == 1 ]] && EXTRA=(--membership-only)
"$GEN" --fund-buckets "$BALANCE" --fund-amount "$FUND_AMOUNT" --subscribers "$SUBSCRIBERS" \
  --membership-out "$MEMBERS" --concurrency 16 "${EXTRA[@]}" \
  --cert certs/hello-nf/cert.pem --key certs/hello-nf/key.pem --ca certs/ca/ca.crt

echo "== 3. shared-bucket membership ($(wc -l <"$MEMBERS") rows) =="
chmod 644 "$MEMBERS" # mktemp makes it 0600; the postgres server process must read it after docker cp
docker cp "$MEMBERS" docker-postgres-chf-1:/tmp/adr0455-members.tsv
pg -c "CREATE TEMP TABLE m (bucket_id text, party_id text, ordinal int);
       COPY m FROM '/tmp/adr0455-members.tsv';
       INSERT INTO balance_mgmt.bucket_related_party (bucket_id, party_id, ordinal)
         SELECT m.bucket_id, m.party_id, m.ordinal FROM m
         WHERE NOT EXISTS (SELECT 1 FROM balance_mgmt.bucket_related_party rp
                           WHERE rp.bucket_id = m.bucket_id AND rp.party_id = m.party_id);
       UPDATE balance_mgmt.bucket SET is_shared = true
         WHERE id IN (SELECT DISTINCT bucket_id FROM m) AND is_shared IS NOT TRUE;"
docker exec docker-postgres-chf-1 rm -f /tmp/adr0455-members.tsv
pg -tAc "SELECT 'shared buckets: ' || count(*) FROM balance_mgmt.bucket WHERE is_shared"
pg -tAc "SELECT 'membership rows: ' || count(*) FROM balance_mgmt.bucket_related_party"
echo "seed complete."
