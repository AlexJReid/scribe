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
  demo/stroke_member_coverage.ndjson \
  demo/stroke_phi_member_coverage.ndjson \
  demo/stroke_balance.json

echo "ingesting 270, 271, 837, 835s to journal, phi enabled"
build/scribe ingest --out demo/stroke.journal \
  --run-id stroke-ingest-demo \
  --phi-vault demo/stroke_phi_vault.sqlite \
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

echo "stitching claims"
build/scribe stitch claims \
  --journal demo/stroke.journal \
  --read-store demo/stroke_read_store.sqlite \
  --notify-out demo/stroke_notifications.ndjson \
  --run-id stroke-stitch-demo \
  --out demo/stroke_aggregates.ndjson

echo "reducing coverage/member context"
build/scribe stitch coverage \
  --journal demo/stroke.journal \
  --read-store demo/stroke_read_store.sqlite \
  --run-id stroke-coverage-demo \
  --out demo/stroke_member_coverage.ndjson

echo "stitching claims again, with phi decoded"
build/scribe stitch claims \
  --journal demo/stroke.journal \
  --read-store demo/stroke_phi_read_store.sqlite \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --include-phi \
  --out demo/stroke_phi_aggregates.ndjson

echo "reducing coverage/member context, with phi decoded"
build/scribe stitch coverage \
  --journal demo/stroke.journal \
  --read-store demo/stroke_phi_read_store.sqlite \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --include-phi \
  --run-id stroke-phi-coverage-demo \
  --out demo/stroke_phi_member_coverage.ndjson

echo "projecting balance"
build/scribe project balance \
  --journal demo/stroke.journal \
  --out demo/stroke_balance.json

echo "done, see demo/ for output"
