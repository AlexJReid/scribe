<img src="./assets/scribe-logo-v2.png" width=200>

# Scribe

`scribe` turns healthcare X12 EDI into auditable events.

Instead of giving you a huge opaque JSON dump, it emits small domain events
with control numbers, segment positions, byte offsets, and run IDs so pipelines
can validate, load, replay, and debug claims/remits without hand-rolling brittle
string parsers.

The normal path is:

```text
X12 files -> journal segments -> aggregate versions/read store
          -> outbox rows -> SQLite delta exports -> downstream stores
```

`parse` is still useful for inspecting a single file, but durable processing
starts at `ingest`.

The parser handles 834 enrollment, 837 claims, 835 remits, and 270/271 eligibility
traffic. Raw PHI can be kept out of normal flows by writing tokenised events and
resolving sensitive values through a separate PHI vault only when required.

> scribe parses X12 syntax and maps selected healthcare EDI facts into journal events. It is not (yet) a full X12/TR3 validator.

## Read next

This README is the quick path. The operational details live in:

- [FAQ](./FAQ.md): operational answers. Start with
  [matching](./FAQ.md#how-does-scribe-match-837-claims-to-835-remits),
  [stitching](./FAQ.md#how-does-it-stitch),
  [progress logs](./FAQ.md#what-do-the-stitch-progress-lines-mean),
  [compressed journals](./FAQ.md#can-journal-segments-be-compressed),
  [PHI](./FAQ.md#how-is-phi-handled), and
  [ingest ordering](./FAQ.md#do-837-files-need-to-be-ingested-in-order).
- [Model notes](./model.md): aggregate/projection boundaries, read-store tables,
  stable IDs, source roots, compressed segments, run IDs, and balance projection.
- [Events](./events.md): mapped event names.

`scribe` is a small C binary. Run it interactively, in shell pipelines, as a
serverless trigger when files arrive in S3, as a cron/batch job, as a K8S job, or
inside common schedulers.

For more advanced usage see [scripts/stroke-demo.sh](./scripts/stroke-demo.sh),
[demo.sh](./demo.sh), or `scribe --help`.

For details, see the FAQ entries on
[stitching](./FAQ.md#how-does-it-stitch),
[progress logs](./FAQ.md#what-do-the-stitch-progress-lines-mean), and
[compressed journals](./FAQ.md#can-journal-segments-be-compressed). For
aggregate/projection boundaries and the storage model, see [model.md](./model.md).

## Examples

Use as much of the pipeline as you need. `parse` is handy for command-line
inspection, `ingest` gives you replayable evidence, and `stitch`/`project` build
durable read stores for applications.

Parse an 837 claim file and filter the emitted event stream:

```bash
scribe parse --type 837 claims.edi \
  | jq 'select(.event_type=="ClaimReferencedSubscriber")'
```

When you want replay and provenance, ingest one or more inputs into a journal:

```bash
scribe ingest --out journal.scribe \
  --source-root inbound \
  --837 inbound/claims.edi \
  --835 inbound/remit.edi
```

For incremental processing, write each source drop as its own journal segment
and stitch only that segment:

```bash
scribe ingest --out journal.d/20260617/drop-001.journal \
  --run-id drop-001 \
  --source-root inbound \
  --837 inbound/claims.edi

scribe stitch claims --journal journal.d/20260617/drop-001.journal \
  --incremental --read-store read_store.sqlite --out changed_claims.ndjson

scribe ingest --out journal.d/20260720/drop-002.journal \
  --run-id drop-002 \
  --source-root inbound \
  --835 inbound/remit.edi

scribe stitch claims --journal journal.d/20260720/drop-002.journal \
  --incremental --read-store read_store.sqlite --out changed_claims.ndjson
```

For larger journal partitions, closed source-drop segments can also be
zstd-compressed:

```bash
scribe ingest --out journal.d/20260617/drop-001.journal.zst \
  --compress zstd \
  --source-root inbound \
  --837 inbound/claims.edi
```

For source roots and segment locator rules, see
[Stable IDs](./model.md#stable-ids). For `.journal.zst` behavior, see
[compressed journals](./FAQ.md#can-journal-segments-be-compressed).

Stitch claim versions by matching 837 claim facts with 835 remittance facts,
then populate read-store indexes:

```bash
scribe stitch claims \
  --journal journal.scribe \
  --read-store read_store.sqlite \
  --out claim_aggregates.ndjson
```

The stitcher records non-PHI source-drop outbox rows in the read store when
`--read-store` is set. See
[Outbox and delta handoff](#outbox-and-delta-handoff) for how consumers pick up
those changes.

Project claim balances from the stitched claim read store:

```bash
scribe project balance \
  --read-store read_store.sqlite \
  --out claim_balances.json
```

## Outbox and delta handoff

The read store is the durable handoff point. When a stitcher commits aggregate
versions, it also writes `SourceDropAggregatesRecorded` rows to
`outbox_notifications`. Those rows are intentionally small and non-PHI: they say
which `source_drop_id` changed, which aggregate family changed, and which outbox
`sequence` a consumer can use as a cursor.

Downstream systems can process changes in batches. The normal flow is:

```text
consumer keeps last_outbox_sequence
  -> export a cursor window from read_store.sqlite
  -> receive scribe_delta.sqlite
  -> read outbox_notifications and aggregate_versions
  -> write DynamoDB/Postgres/search/cache/etc
  -> store the exported to_sequence as the new cursor
```

To hand a batch to another process as SQLite instead of JSON/HTTP calls, export
a delta database:

```bash
scribe export delta \
  --read-store read_store.sqlite \
  --after-sequence 0 \
  --limit 1000 \
  --out scribe_delta.sqlite
```

`--limit` is the maximum number of outbox rows to put in one delta file. It is a
batch-size control, not a data filter: every aggregate version referenced by the
selected source-drop notifications is included. Consumers store the exported
`to_sequence` from the delta metadata and use it as the next
`--after-sequence`; repeat until an export returns zero notifications.

The delta file is a disposable transfer artifact. It contains `metadata`,
`outbox_notifications`, `source_drops`, and normalized `aggregate_versions`
tables for the exported cursor window. It does not replace the read-store
outbox; if delivery fails, export the same sequence window again.

## Build

Release binaries are attached to
[the latest GitHub release](https://github.com/AlexJReid/scribe/releases/latest).

The case study will build if needed. Local builds need SQLite, OpenSSL, and
zstd development packages. See [CI](./.github/workflows) for exact packages.

```sh
./scripts/stroke-demo.sh
```

or

```sh
cmake -S . -B build
cmake --build build
```

Generate a local throughput workload without checking in bulk EDI files:

```sh
scripts/throughput-test.sh
TYPE=835 FILE_COUNT=5000 KEEP=1 scripts/throughput-test.sh
```

## Shape

- Inputs: 834 enrollment, 837 claims, 835 remits, 270/271 eligibility
- Events: small auditable facts with source transaction, control numbers,
  segment index, byte offset, and optional run ID
- Journal: immutable binary evidence stream, either one segment file or a
  directory of raw `.journal` and/or compressed `.journal.zst` segment files
- PHI vault: raw PHI resolver, separate from normal stores
- Read store: indexes, versioned aggregate snapshots, and latest rows
- Debug/output streams: aggregate/version NDJSON and balances.
  Stitch aggregate NDJSON is for inspection; applications should consume the
  read store.
- Outbox: durable source-drop notifications in the read store.
- Delta export: portable SQLite transfer files for downstream fan-out.

SQLite is a practical local store for the vault and read stores, a fast scratch
area for batch transforms, and a portable transfer format for delta handoff. The
storage boundary is narrow enough to add a managed database later where a
deployment needs one.

## Demo

The synthetic _stroke_ case study lives in
[tests/fixtures/stroke_encounter/](./tests/fixtures/stroke_encounter/). Generated
reference output lives in [demo/](./demo).

```sh
./scripts/stroke-demo.sh
./demo.sh
```

Inspect the claim latest table:

```sh
sqlite3 -header -column demo/stroke_read_store.sqlite "
select aggregate_id, version, state_json
from claim_aggregate_latest
order by aggregate_id;
"
```

See [scripts/stroke-demo.sh](./scripts/stroke-demo.sh) and [demo.sh](./demo.sh)
for the full ingest, stitch, coverage, PHI, and balance command lines.

## PHI

Default flows stay tokenised. Use `--include-phi --phi-vault ...` only for
controlled PHI read stores.

When PHI is resolved into a PHI read store or exported delta, Scribe assumes the
handoff is encrypted at the transport/storage layer and that recipient systems
are explicitly authorized to receive PHI. Those systems must have their own
auditing, access control, retention, and operational controls. The PHI vault can
audit resolution; downstream custody belongs to the receiving system.

All PHI-looking fixture values are made up. The stroke case study is only
inspired by a UK, non-US healthcare episode that I personally had. IDs, payer
details, dates, amounts, and EDI content are made up.

## More

- [FAQ.md](./FAQ.md): operational questions, including matching, stitch modes,
  progress logs, compressed journals, PHI, deployment, and ordering.
- [model.md](./model.md): storage model, aggregate/projection boundaries,
  stable IDs, runs, PHI, and balance projection.
- [events.md](./events.md): event names
- [tests/fixtures/stroke_encounter/README.md](./tests/fixtures/stroke_encounter/README.md): fixture map

## License

MIT. See `LICENSE`.
