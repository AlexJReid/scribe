#!/bin/sh
set -eu

cd "$(dirname "$0")"

missing=0

need_file() {
    if [ ! -f "$1" ]; then
        echo "missing: $1" >&2
        missing=1
    fi
}

need_file demo/stroke.journal
need_file demo/stroke_phi_vault.sqlite
need_file demo/stroke_read_store.sqlite
need_file demo/stroke_phi_read_store.sqlite
need_file demo/stroke_aggregates.ndjson
need_file demo/stroke_phi_aggregates.ndjson
need_file demo/stroke_notifications.ndjson
need_file demo/stroke_balance.json

if [ "$missing" -ne 0 ]; then
    echo "Run ./scripts/stroke-demo.sh first to populate demo/." >&2
    exit 1
fi

echo "Stroke demo artifacts"
ls -lh \
  demo/stroke.journal \
  demo/stroke_phi_vault.sqlite \
  demo/stroke_read_store.sqlite \
  demo/stroke_phi_read_store.sqlite \
  demo/stroke_aggregates.ndjson \
  demo/stroke_phi_aggregates.ndjson \
  demo/stroke_notifications.ndjson \
  demo/stroke_balance.json

echo
echo "Balance totals"
sed -n 's/.*"totals":{"total_billed":"\([^"]*\)","payer_paid":"\([^"]*\)","contractual_adjustments":"\([^"]*\)","patient_responsibility":"\([^"]*\)".*"current_balance":"\([^"]*\)"}.*/total_billed=\1 payer_paid=\2 contractual_adjustments=\3 patient_responsibility=\4 current_balance=\5/p' \
  demo/stroke_balance.json

echo
echo "Latest claim aggregates: tokenised read store"
if command -v sqlite3 >/dev/null 2>&1; then
    sqlite3 -header -column demo/stroke_read_store.sqlite "
select
  version,
  json_extract(state_json, '$.state.claim_type') as claim_type,
  json_extract(state_json, '$.keys.claim_id') as claim_token,
  json_extract(state_json, '$.keys.payer_claim_control_number') as payer_control_token,
  json_extract(state_json, '$.keys.patient_id') as patient_token,
  json_extract(state_json, '$.state.submitted_service_line_count') as submitted_lines,
  json_extract(state_json, '$.state.remittance_service_line_count') as remit_lines,
  json_extract(state_json, '$.source_drop_id') as source_drop
from claim_aggregate_latest
order by aggregate_id;
"
else
    echo "sqlite3 not found; skipping read-store summary"
fi

echo
echo "Latest claim aggregates: PHI read store"
if command -v sqlite3 >/dev/null 2>&1; then
    sqlite3 -header -column demo/stroke_phi_read_store.sqlite "
select
  version,
  json_extract(state_json, '$.state.claim_type') as claim_type,
  json_extract(state_json, '$.keys.claim_id') as claim_id,
  json_extract(state_json, '$.keys.claim_id_token') as claim_token,
  json_extract(state_json, '$.keys.payer_claim_control_number') as payer_control,
  json_extract(state_json, '$.keys.payer_claim_control_number_token') as payer_control_token,
  json_extract(state_json, '$.keys.patient_id') as patient_id,
  json_extract(state_json, '$.keys.patient_id_token') as patient_token,
  json_extract(state_json, '$.keys.patient_last_name_or_org') as last_name,
  json_extract(state_json, '$.keys.patient_first_name') as first_name
from claim_aggregate_latest
order by aggregate_id;
"
else
    echo "sqlite3 not found; skipping PHI read-store summary"
fi

echo
echo "First notifications"
sed -n '1,3p' demo/stroke_notifications.ndjson
