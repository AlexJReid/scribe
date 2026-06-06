#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

cmake -S . -B build
cmake --build build

rm -f \
  demo/stroke.journal \
  demo/stroke_phi_vault.sqlite \
  demo/stroke_phi_vault.sqlite-shm \
  demo/stroke_phi_vault.sqlite-wal \
  demo/stroke_read_store.sqlite \
  demo/stroke_read_store.sqlite-shm \
  demo/stroke_read_store.sqlite-wal \
  demo/stroke_phi_read_store.sqlite \
  demo/stroke_phi_read_store.sqlite-shm \
  demo/stroke_phi_read_store.sqlite-wal \
  demo/stroke_aggregates.ndjson \
  demo/stroke_notifications.ndjson \
  demo/stroke_phi_aggregates.ndjson \
  demo/stroke_balance.json

build/scribe journal --out demo/stroke.journal \
  --run-id stroke-ingest-demo \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --charges tests/fixtures/stroke_encounter/charge_transactions.ndjson \
  --837 tests/fixtures/stroke_encounter/facility_837.edi \
  --837 tests/fixtures/stroke_encounter/professional_837.edi \
  --837 tests/fixtures/stroke_encounter/rehab_837.edi \
  --837 tests/fixtures/stroke_encounter/neurology_837.edi \
  --835 tests/fixtures/stroke_encounter/facility_835.edi \
  --835 tests/fixtures/stroke_encounter/professional_835.edi \
  --835 tests/fixtures/stroke_encounter/rehab_835.edi \
  --835 tests/fixtures/stroke_encounter/neurology_835.edi \
  --834 tests/fixtures/stroke_encounter/coverage_834.edi \
  --270 tests/fixtures/stroke_encounter/eligibility_270.edi \
  --271 tests/fixtures/stroke_encounter/eligibility_271.edi

build/scribe stitch \
  --journal demo/stroke.journal \
  --encounter-id ENC-SYN-STROKE-001 \
  --read-store demo/stroke_read_store.sqlite \
  --notify-out demo/stroke_notifications.ndjson \
  --run-id stroke-stitch-demo \
  --out demo/stroke_aggregates.ndjson

build/scribe stitch \
  --journal demo/stroke.journal \
  --encounter-id ENC-SYN-STROKE-001 \
  --read-store demo/stroke_phi_read_store.sqlite \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --include-phi \
  --out demo/stroke_phi_aggregates.ndjson

build/scribe project --projection balance \
  --journal demo/stroke.journal \
  --encounter-id ENC-SYN-STROKE-001 \
  --out demo/stroke_balance.json
