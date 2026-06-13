#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

usage() {
  cat <<'USAGE'
usage: scripts/throughput-test.sh [file-count]

Generates synthetic EDI files on demand, writes a path list, and ingests that
list into one temporary journal. Generated EDI and journal output are removed
unless KEEP=1 is set.

Environment:
  FILE_COUNT   Number of generated EDI files. Default: 1000.
  TYPE         Transaction type: 270, 271, 834, 835, or 837. Default: 837.
  BUILD_DIR    CMake build directory. Default: build.
  PYTHON       Python interpreter. Default: python3.
  SKIP_BUILD   Set to 1 to skip cmake configure/build.
  WORKDIR      Parent directory for generated workload. Default: mktemp under /tmp.
  KEEP         Set to 1 to keep generated files and journal.
  COMPRESS     Set to 1 to write a zstd-compressed .journal.zst segment.

Examples:
  scripts/throughput-test.sh
  FILE_COUNT=5000 scripts/throughput-test.sh
  TYPE=835 FILE_COUNT=2000 KEEP=1 scripts/throughput-test.sh
  COMPRESS=1 FILE_COUNT=5000 KEEP=1 scripts/throughput-test.sh
USAGE
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  usage
  exit 0
fi

FILE_COUNT="${1:-${FILE_COUNT:-1000}}"
BUILD_DIR="${BUILD_DIR:-build}"
PYTHON="${PYTHON:-python3}"
SKIP_BUILD="${SKIP_BUILD:-0}"
KEEP="${KEEP:-0}"
TYPE="${TYPE:-837}"
COMPRESS="${COMPRESS:-0}"

case "$FILE_COUNT" in
  ''|*[!0-9]*)
    echo "FILE_COUNT must be a positive integer" >&2
    exit 2
    ;;
esac
if [ "$FILE_COUNT" -lt 1 ]; then
  echo "FILE_COUNT must be at least 1" >&2
  exit 2
fi
case "$TYPE" in
  270|271|834|835|837)
    ;;
  *)
    echo "TYPE must be one of 270, 271, 834, 835, or 837" >&2
    exit 2
    ;;
esac

if [ -n "${WORKDIR:-}" ]; then
  mkdir -p "$WORKDIR"
  workdir=$(mktemp -d "$WORKDIR/run.XXXXXX")
else
  workdir=$(mktemp -d "${TMPDIR:-/tmp}/scribe-throughput.XXXXXX")
fi

cleanup() {
  if [ "$KEEP" != "1" ]; then
    rm -rf "$workdir"
  fi
}
trap cleanup EXIT HUP INT TERM

edi_dir="$workdir/edi"
journal_path="$workdir/throughput.journal"
compress_args=
if [ "$COMPRESS" = "1" ]; then
  journal_path="$journal_path.zst"
  compress_args="--compress zstd"
fi
list_path="$workdir/input-files.txt"
mkdir -p "$edi_dir"

if [ "$SKIP_BUILD" != "1" ]; then
  cmake -S . -B "$BUILD_DIR"
  cmake --build "$BUILD_DIR"
fi

scribe_bin="$BUILD_DIR/scribe"
if [ ! -x "$scribe_bin" ]; then
  echo "missing executable: $scribe_bin" >&2
  echo "run cmake --build $BUILD_DIR or leave SKIP_BUILD unset" >&2
  exit 1
fi

echo "generating $FILE_COUNT EDI files under $edi_dir"
generate_start=$(date +%s)
input_bytes=$("$PYTHON" scripts/generate-throughput-x12.py \
  --type "$TYPE" \
  --count "$FILE_COUNT" \
  --out-dir "$edi_dir" \
  --list-out "$list_path")
generate_end=$(date +%s)

echo "ingesting $FILE_COUNT $TYPE files into $journal_path"
ingest_start=$(date +%s)
"$scribe_bin" ingest \
  --out "$journal_path" \
  --run-id "throughput-$TYPE-$FILE_COUNT" \
  --source-root "$workdir" \
  $compress_args \
  "--$TYPE-list" "$list_path"
ingest_end=$(date +%s)

generate_elapsed=$((generate_end - generate_start))
ingest_elapsed=$((ingest_end - ingest_start))
if [ "$generate_elapsed" -lt 1 ]; then
  generate_elapsed=1
fi
if [ "$ingest_elapsed" -lt 1 ]; then
  ingest_elapsed=1
fi
journal_bytes=$(wc -c < "$journal_path" | tr -d ' ')
files_per_second=$(awk -v n="$FILE_COUNT" -v s="$ingest_elapsed" 'BEGIN { printf "%.2f", n / s }')
mb_per_second=$(awk -v b="$input_bytes" -v s="$ingest_elapsed" 'BEGIN { printf "%.2f", (b / 1048576) / s }')

echo "workdir: $workdir"
echo "type: $TYPE"
echo "generated_files: $FILE_COUNT"
echo "input_bytes: $input_bytes"
echo "journal_bytes: $journal_bytes"
echo "generate_seconds: $generate_elapsed"
echo "ingest_seconds: $ingest_elapsed"
echo "ingest_files_per_second: $files_per_second"
echo "ingest_input_mib_per_second: $mb_per_second"
if [ "$KEEP" = "1" ]; then
  echo "kept generated workload: $workdir"
fi
