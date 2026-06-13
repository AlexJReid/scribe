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
  segment_path="$segment_dir/$run_id.journal.zst"

  mkdir -p "$segment_dir"
  echo "writing source drop segment: $segment_path ($type $path)"
  build/scribe ingest --out "$segment_path" \
    --run-id "$run_id" \
    --compress zstd \
    --source-root tests/fixtures \
    --phi-vault demo/stroke_phi_vault.sqlite \
    "--$type" "$path"
}

append_claim_delta() {
  segment_path="$1"
  read_store="$2"
  out_path="$3"
  run_id="$4"
  delta_path="demo/.stroke_claim_delta.ndjson"
  shift 4

  build/scribe stitch claims \
    --journal "$segment_path" \
    --incremental \
    --read-store "$read_store" \
    --run-id "$run_id" \
    --out "$delta_path" \
    "$@"
  cat "$delta_path" >> "$out_path"
}

append_coverage_delta() {
  segment_path="$1"
  read_store="$2"
  out_path="$3"
  run_id="$4"
  delta_path="demo/.stroke_coverage_delta.ndjson"
  shift 4

  build/scribe stitch coverage \
    --journal "$segment_path" \
    --incremental \
    --read-store "$read_store" \
    --run-id "$run_id" \
    --out "$delta_path" \
    "$@"
  cat "$delta_path" >> "$out_path"
}

echo "writing initial partitioned source drops, with PHI mappings stored in vault"
append_source_drop 20260601 stroke-drop-coverage-834 834 tests/fixtures/stroke_encounter/coverage_834.edi
append_source_drop 20260602 stroke-drop-eligibility-270 270 tests/fixtures/stroke_encounter/eligibility_270.edi
append_source_drop 20260603 stroke-drop-eligibility-271 271 tests/fixtures/stroke_encounter/eligibility_271.edi
append_source_drop 20260617 stroke-drop-facility-837 837 tests/fixtures/stroke_encounter/facility_837.edi

echo
echo "------- initial journal segments created --------"
echo

echo "seeding resume demo store from facility 837 segment"
build/scribe stitch claims \
  --journal demo/stroke.journal.d/20260617/stroke-drop-facility-837.journal.zst \
  --incremental \
  --read-store demo/stroke_resume_read_store.sqlite \
  --run-id stroke-resume-facility-837 \
  --out demo/stroke_resume_first.ndjson

append_source_drop 20260617 stroke-drop-professional-837 837 tests/fixtures/stroke_encounter/professional_837.edi
append_source_drop 20260701 stroke-drop-rehab-837 837 tests/fixtures/stroke_encounter/rehab_837.edi
append_source_drop 20260715 stroke-drop-neurology-837 837 tests/fixtures/stroke_encounter/neurology_837.edi
append_source_drop 20260720 stroke-drop-facility-835 835 tests/fixtures/stroke_encounter/facility_835.edi

echo
echo "resuming demo store by applying facility 835 segment"
build/scribe stitch claims \
  --journal demo/stroke.journal.d/20260720/stroke-drop-facility-835.journal.zst \
  --incremental \
  --read-store demo/stroke_resume_read_store.sqlite \
  --run-id stroke-resume-facility-835 \
  --out demo/stroke_resume_append.ndjson

append_source_drop 20260720 stroke-drop-professional-835 835 tests/fixtures/stroke_encounter/professional_835.edi
append_source_drop 20260728 stroke-drop-rehab-835 835 tests/fixtures/stroke_encounter/rehab_835.edi
append_source_drop 20260805 stroke-drop-neurology-835 835 tests/fixtures/stroke_encounter/neurology_835.edi

echo "stitching claim segments incrementally into tokenised read store"
: > demo/stroke_aggregates.ndjson
append_claim_delta demo/stroke.journal.d/20260617/stroke-drop-facility-837.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo
append_claim_delta demo/stroke.journal.d/20260617/stroke-drop-professional-837.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo
append_claim_delta demo/stroke.journal.d/20260701/stroke-drop-rehab-837.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo
append_claim_delta demo/stroke.journal.d/20260715/stroke-drop-neurology-837.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo
append_claim_delta demo/stroke.journal.d/20260720/stroke-drop-facility-835.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo
append_claim_delta demo/stroke.journal.d/20260720/stroke-drop-professional-835.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo
append_claim_delta demo/stroke.journal.d/20260728/stroke-drop-rehab-835.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo
append_claim_delta demo/stroke.journal.d/20260805/stroke-drop-neurology-835.journal.zst demo/stroke_read_store.sqlite demo/stroke_aggregates.ndjson stroke-stitch-demo

echo "stitching coverage segments incrementally into tokenised read store"
: > demo/stroke_member_coverage.ndjson
append_coverage_delta demo/stroke.journal.d/20260601/stroke-drop-coverage-834.journal.zst demo/stroke_read_store.sqlite demo/stroke_member_coverage.ndjson stroke-coverage-demo
append_coverage_delta demo/stroke.journal.d/20260602/stroke-drop-eligibility-270.journal.zst demo/stroke_read_store.sqlite demo/stroke_member_coverage.ndjson stroke-coverage-demo
append_coverage_delta demo/stroke.journal.d/20260603/stroke-drop-eligibility-271.journal.zst demo/stroke_read_store.sqlite demo/stroke_member_coverage.ndjson stroke-coverage-demo

echo "stitching claim segments incrementally into PHI-resolved read store"
: > demo/stroke_phi_aggregates.ndjson
append_claim_delta demo/stroke.journal.d/20260617/stroke-drop-facility-837.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_claim_delta demo/stroke.journal.d/20260617/stroke-drop-professional-837.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_claim_delta demo/stroke.journal.d/20260701/stroke-drop-rehab-837.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_claim_delta demo/stroke.journal.d/20260715/stroke-drop-neurology-837.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_claim_delta demo/stroke.journal.d/20260720/stroke-drop-facility-835.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_claim_delta demo/stroke.journal.d/20260720/stroke-drop-professional-835.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_claim_delta demo/stroke.journal.d/20260728/stroke-drop-rehab-835.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_claim_delta demo/stroke.journal.d/20260805/stroke-drop-neurology-835.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_aggregates.ndjson stroke-phi-stitch-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi

echo "stitching coverage segments incrementally into PHI-resolved read store"
: > demo/stroke_phi_member_coverage.ndjson
append_coverage_delta demo/stroke.journal.d/20260601/stroke-drop-coverage-834.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_member_coverage.ndjson stroke-phi-coverage-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_coverage_delta demo/stroke.journal.d/20260602/stroke-drop-eligibility-270.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_member_coverage.ndjson stroke-phi-coverage-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi
append_coverage_delta demo/stroke.journal.d/20260603/stroke-drop-eligibility-271.journal.zst demo/stroke_phi_read_store.sqlite demo/stroke_phi_member_coverage.ndjson stroke-phi-coverage-demo --phi-vault demo/stroke_phi_vault.sqlite --include-phi

rm -f demo/.stroke_claim_delta.ndjson
rm -f demo/.stroke_coverage_delta.ndjson

echo "projecting balance from claim read store"
build/scribe project balance \
  --read-store demo/stroke_read_store.sqlite \
  --out demo/stroke_balance.json

echo "exporting portable SQLite deltas from read-store outboxes"
build/scribe export delta \
  --read-store demo/stroke_read_store.sqlite \
  --after-sequence 0 \
  --limit 1000 \
  --out demo/stroke_delta.sqlite
build/scribe export delta \
  --read-store demo/stroke_phi_read_store.sqlite \
  --after-sequence 0 \
  --limit 1000 \
  --out demo/stroke_phi_delta.sqlite

echo "done, see demo/ for generated case-study output"
