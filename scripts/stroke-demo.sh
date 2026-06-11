#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

cmake -S . -B build
cmake --build build

rm -rf demo
mkdir -p demo

append_source_drop() {
  arrival_date="$1"
  run_id="$2"
  type="$3"
  path="$4"
  segment_dir="demo/stroke.journal.d/$arrival_date"
  segment_path="$segment_dir/$run_id.journal"

  mkdir -p "$segment_dir"
  echo "writing source drop segment: $segment_path ($type $path)"
  build/scribe ingest --out "$segment_path" \
    --run-id "$run_id" \
    --phi-vault demo/stroke_phi_vault.sqlite \
    "--$type" "$path"
}

echo "writing partitioned stroke journal source drops, phi vault enabled"
append_source_drop 20260601 stroke-drop-coverage-834 834 tests/fixtures/stroke_encounter/coverage_834.edi
append_source_drop 20260602 stroke-drop-eligibility-270 270 tests/fixtures/stroke_encounter/eligibility_270.edi
append_source_drop 20260603 stroke-drop-eligibility-271 271 tests/fixtures/stroke_encounter/eligibility_271.edi
append_source_drop 20260617 stroke-drop-facility-837 837 tests/fixtures/stroke_encounter/facility_837.edi
append_source_drop 20260617 stroke-drop-professional-837 837 tests/fixtures/stroke_encounter/professional_837.edi
append_source_drop 20260701 stroke-drop-rehab-837 837 tests/fixtures/stroke_encounter/rehab_837.edi
append_source_drop 20260715 stroke-drop-neurology-837 837 tests/fixtures/stroke_encounter/neurology_837.edi
append_source_drop 20260720 stroke-drop-facility-835 835 tests/fixtures/stroke_encounter/facility_835.edi
append_source_drop 20260720 stroke-drop-professional-835 835 tests/fixtures/stroke_encounter/professional_835.edi
append_source_drop 20260728 stroke-drop-rehab-835 835 tests/fixtures/stroke_encounter/rehab_835.edi
append_source_drop 20260805 stroke-drop-neurology-835 835 tests/fixtures/stroke_encounter/neurology_835.edi

echo "stitching claims"
build/scribe stitch claims \
  --journal demo/stroke.journal.d \
  --read-store demo/stroke_read_store.sqlite \
  --notify-out demo/stroke_notifications.ndjson \
  --run-id stroke-stitch-demo \
  --out demo/stroke_aggregates.ndjson

echo "reducing coverage/member context"
build/scribe stitch coverage \
  --journal demo/stroke.journal.d \
  --read-store demo/stroke_read_store.sqlite \
  --run-id stroke-coverage-demo \
  --out demo/stroke_member_coverage.ndjson

echo "stitching claims again, with phi decoded"
build/scribe stitch claims \
  --journal demo/stroke.journal.d \
  --read-store demo/stroke_phi_read_store.sqlite \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --include-phi \
  --out demo/stroke_phi_aggregates.ndjson

echo "reducing coverage/member context, with phi decoded"
build/scribe stitch coverage \
  --journal demo/stroke.journal.d \
  --read-store demo/stroke_phi_read_store.sqlite \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --include-phi \
  --run-id stroke-phi-coverage-demo \
  --out demo/stroke_phi_member_coverage.ndjson

echo "projecting balance"
build/scribe project balance \
  --journal demo/stroke.journal.d \
  --out demo/stroke_balance.json

echo "done, see demo/ for output"
