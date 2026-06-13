# FAQ

## Why does scribe exist?

Health EDI is full of painful old school formats, fragmented workflows,
proprietary systems, org-specific behaviour, and enterprise integration
BS. Claims, remits, eligibility responses, and enrollment files often
arrive separately, may get corrected and have to be correlated later.

scribe exists to turn those dropped X12 files into small auditable events that can be
stitched, projected, indexed, replayed, and queried without forcing every
consumer to understand EDI directly. 

The goal is not to build another heavyweight integration platform. There is no huge SQL schema requiring lots of joins. 

The goal is a small, high-performance Unix-style tool:

- X12 in
- Auditable events out
- Versioned read store projections when there is enough data
- Open file formats and streams
- Portable deployment
- No required runtime (other than libc)

Run it from the shell, from cron, in a container, in Lambda/serverless handlers,
on a batch host, or inside a scheduler. Let other systems consume the outputs in
whatever language they already use.

## Why is scribe written in C?

The goal is not to force users into C.

The goal is to build a small, portable Unix-style tool that can run almost
anywhere and emit open formats that any language can consume.

scribe is implemented in C because it gives the project:

- Small binaries
- Fast startup
- Low memory overhead; streams not in-memory state
- High-throughput parsing and journal reads
- Minimal deployment dependencies
- Straightforward streaming parsers
- Works on Linux, macOS, Windows, containers, schedulers, cron jobs,
  and serverless environments.

The public contract is **not** a C API.

The contract is:

- X12 files and streams
- JSON and NDJSON
- Journal segments
- SQLite read stores
- Open, documented event formats

Python, Go, Java, C#, Rust, shell scripts, Spark, DuckDB, jq, and other tools can
all consume those outputs.

In other words:

```text
X12 in -> auditable events out
```

rather than:

```text
link against a huge C/C++ library with lots of dependencies
```

## How does scribe match 837 claims to 835 remits?

Claim matching starts with the claim id shared by the X12 documents. The 837
`CLM01` value and the 835 `CLP01` value are treated as the same claim id
namespace. In the default non-PHI path, that id is tokenised and the aggregate
id is based on the token:

```text
claim:<tokenised CLM01/CLP01>
```

When an 835 includes `CLP07`, scribe also records it as the payer claim control
number. That value is indexed as another durable key for the same claim
aggregate, so later journal events can find the aggregate by either the original
claim id or the payer control number.

Service lines are matched inside the claim aggregate. The stitcher first tries
procedure code plus charge amount. If matching service dates are present, the
line records `match_method: "procedure_charge_date"`. Without a date match it
records `procedure_charge`. If procedure and charge do not identify a submitted
line, the stitcher falls back to service line order. A remit-only line is kept
with `match_method: "created_from_remittance"` instead of being dropped.

## How does it stitch?

Ingest writes small, tokenised journal events with source drop, run, control
number, segment, and byte-offset provenance. Stitching reads those journal
events and materializes aggregate versions.

There are two stitch modes:

- Full replay: omit `--incremental`. The stitcher reads the supplied journal
  file or journal directory and rebuilds aggregate output from the event stream.
- Incremental append: pass `--incremental --read-store store.sqlite`. The
  supplied journal is treated as the new source-drop segment. The stitcher
  indexes the new events, marks affected aggregate ids dirty, hydrates only
  those aggregates from the read store, applies the new events, and writes the
  next aggregate versions.

Hydration is quick because incremental reduce does not replay old journal
segments to reconstruct prior state. The shuffle pass records which aggregate
ids are dirty for the new source drop, then the reduce pass loads each dirty
aggregate's latest `state_json` snapshot from the read store, applies only the
new journal events, and persists the next version. In practice this makes normal
processing proportional to the new drop plus the aggregates it touches, not to
the full journal history. The relevant read-store tables are summarized in
[model.md](./model.md#storage).

The NDJSON written by `--out` is an inspection/debug stream for the versions
changed by that run. Applications should read the read-store tables for durable
latest and versioned aggregate state.

## Why SQLite? Won't that limit scaling?

SQLite is not meant to sit behind user-facing applications here. In scribe it is
the local operational surface: a vault store, aggregate materialization, index
store, outbox cursor, scratch area for batch work, and portable delta format.

That is a good fit for a small C binary. SQLite gives atomic commits, stable
files, indexes, transactions, simple inspection with `sqlite3`, and mature
drivers in almost every runtime without requiring a database server next to
every ingest or stitch job.

Serving systems can consume the outbox or delta files and load Postgres,
DynamoDB, search indexes, caches, warehouses, or whatever they use to serve
traffic. SQLite is the source-of-truth handoff and batch materialization layer,
not a requirement that downstream applications query scribe's read store live.
The storage boundary stays narrow enough to add a managed database later where a
deployment needs one.

## Are SQLite delta files the integration contract?

Yes. For downstream systems, the intended contract is the read-store outbox plus
portable SQLite delta files.

That is deliberate. A SQLite delta gives consumers a transactional batch with a
schema, indexes, cursor metadata, and queryable rows in one file. Consumers can
inspect it with `sqlite3`, attach it to another SQLite connection, read it from
Python/Go/Java/Rust, or load it into DuckDB without running a Scribe service.
Compared with a JSON API, this avoids per-aggregate request loops, pagination
edge cases, partial-batch ambiguity, and another always-on auth/retry surface.
The API boundary, if one is added later, can serve or authorize delta files
rather than becoming the data format itself.

When `--read-store` is set, stitchers record non-PHI
`SourceDropAggregatesRecorded` rows in `outbox_notifications`. An outbox row is
a pointer, not the payload: it records the outbox `sequence`, `source_drop_id`,
aggregate family, stitch run id, source run id, and count of aggregate versions
created for that source drop.

To convey data to another process, export an outbox cursor window:

```sh
scribe export delta \
  --read-store store.sqlite \
  --after-sequence 123 \
  --limit 1000 \
  --out delta.sqlite
```

The exported delta SQLite file contains:

- `metadata`: schema version, `after_sequence`, `to_sequence`, and row counts
- `outbox_notifications`: the exported source-drop notifications
- `source_drops`: provenance rows for referenced source drops
- `aggregate_versions`: normalized claim and coverage aggregate versions

Delta files exported from the normal read store stay tokenised. A delta exported
from a PHI read store is PHI-bearing even though the outbox rows are still small
pointers. PHI handoffs are encrypted at the transport/storage layer and are sent
only to systems authorized to receive PHI; Scribe assumes those systems have
their own auditing, access control, retention, and operational controls.

`--limit` is the maximum number of outbox rows in one export. It is not a data
filter: every aggregate version referenced by the selected source-drop
notifications is included.

Consumers should treat the delta file as a disposable transfer artifact:

```text
read last processed outbox sequence
  -> export delta.sqlite from the read store
  -> load delta.sqlite with sqlite3, Python, Go, DuckDB, etc
  -> write the target store
  -> persist delta.metadata.to_sequence as the next cursor
```

If delivery or target writes fail, re-export the same sequence window and retry.
The durable cursor and source of truth remain in the read store's
`outbox_notifications` table; the delta file does not replace it.

## What do the stitch progress lines mean?

`stitch claims` and `stitch coverage` write progress to stderr. The verbs are
the same for both aggregate families:

The example below is incremental mode: `--incremental --read-store ...` treats
the supplied journal as the new source-drop segment and uses a shuffle pass plus
a reduce pass. Full replay mode is what you get when `--incremental` is omitted:
the stitcher reads the supplied journal file or directory once and rebuilds
aggregate output directly from the event stream.

```text
scribe stitch coverage: shuffle pass journal=...
scribe stitch coverage: source_drop=271:000000111:111:0001 source_run=stroke-drop-eligibility-271
scribe stitch coverage: reduce pass journal=...
scribe stitch coverage: hydrate aggregate=member_coverage:<id> version=2
scribe stitch coverage: emit aggregate=member_coverage:<id> version=3 source_drop=271:000000111:111:0001
scribe stitch coverage: done events=12 dirty_routes=24 aggregates=1 status=ok
```

- `start`: command configuration: replay or incremental mode, journal path,
  read-store path, and debug output path.
- `shuffle pass`: incremental pass 1. The stitcher indexes the new segment and
  records dirty aggregate routes without emitting aggregate versions yet.
- `source_drop`: the X12 transaction/source-drop identity currently being read,
  plus the ingest run id copied from the journal.
- `reduce pass`: incremental pass 2. The stitcher re-reads the same segment,
  hydrates affected aggregates, applies the new events, and emits versions.
- `hydrate aggregate`: an existing latest aggregate snapshot was loaded from
  the read store before applying the new source drop. Absence of this line
  usually means the aggregate is new for this read store.
- `emit aggregate`: a changed aggregate version was written to the debug
  NDJSON stream and read store. The version is the new aggregate version.
- `done`: final counters and status. `events` is the journal event count for
  the current pass, `dirty_routes` is the number of event-to-aggregate route
  marks considered during incremental routing, and `aggregates` is the number
  of aggregate versions emitted. `status=ok` means the command returned
  `X12_OK`.

In incremental mode it is normal to see the same `source_drop` once in the
shuffle pass and once in the reduce pass. It is also normal for `dirty_routes`
to be larger than `events` because one event can mark more than one key or
aggregate route.

## What is a deployment shape?

`scribe` is a CLI tool built for Unix-style composition: run it in shell
pipelines, cron or batch jobs, serverless triggers, containers, schedulers, or
small long-lived workers. The diagram below is one practical deployment shape,
not a required topology.

That shape is one Linux host with local NVMe storage. A watched SFTP drop
receives provider X12 files, a watcher starts a `scribe ingest` run, and ingest
parses the file into a new immutable `.journal` segment with a run id and
source-drop provenance.

After that, `scribe stitch` and `scribe project` run incrementally against the
new segment. They update a local read store, indexes, latest aggregate rows, and
the outbox. Consumers read current state from the read store and receive
notifications from the outbox; they do not read raw X12 files directly.

![Scribe incremental journal deployment](./assets/deployment.png)

The journal remains the durable evidence stream. The read store, indexes, and
notification state are derived cache/materialization surfaces that can be
rebuilt by replaying journal segments. A PHI vault, when used, sits alongside
the normal stores rather than inside the default non-PHI path.

## Why have a journal?

The journal is the evidence store. It keeps the parsed facts as an append-only
stream with source drop, run, control number, segment, and byte-offset context.
That gives later processors one stable input for replay, audit, indexing,
aggregate rebuilds, balance projection, and debugging.

The read store is intentionally separate. It is the materialized lookup surface:
event indexes, aggregate keys, versioned snapshots, latest rows, and dirty
routing. If a read model is wrong or a new projection is needed, the journal is
the source to replay from.

## Why is the journal binary?

The binary format is a framed event stream, not a database. Each file has a
scribe journal header and length-prefixed records with typed fields and compact
field ids for common event names. That makes appends and reads simple in C,
lets readers skip or seek by stored record length, and keeps byte locators
stable for audit/index rows. The journal is verbose, but compresses well.

Text output still exists where it is useful: `parse` emits JSON for inspection,
and stitch/project commands can write NDJSON debug streams. The durable evidence
path uses the binary journal so normal processors do not depend on reparsing
large JSON dumps.

## Can journal segments be compressed?

Yes. Closed source-drop segments can be written with zstd:

```sh
scribe ingest --out journal.d/20260617/drop-001.journal.zst \
  --compress zstd \
  --source-root inbound \
  --837 inbound/claims.edi
```

Compressed segments are closed segments. `--compress zstd` does not support
`--append`, so keep any active append target as a raw `.journal` file and write
closed drops as `.journal.zst`. Stitch and project readers accept raw `.journal`
and compressed `.journal.zst` files in the same journal directory.

## How is PHI handled?

Default flows stay tokenised. Identifier-like values are hashed into stable
tokens by namespace, raw names and ids are omitted from normal read stores, and
aggregate keys use tokens.

When raw PHI is needed, use a separate PHI vault and an explicit
`--include-phi --phi-vault ...` workflow. The vault maps `namespace + token` to
the raw value so controlled readers can resolve it without making the normal
journal, aggregate store, or outbox PHI-bearing by default.

Resolving PHI is an authorization boundary. PHI handoffs are encrypted at the
transport/storage layer. Scribe can audit vault resolution, but it assumes every
system allowed to receive resolved PHI has its own audit logs, access controls,
retention policy, and operational safeguards.

## Do 837 files need to be ingested in order?

No. The journal is append-only evidence, so source drops should usually be
ingested in arrival order. Matching uses stable domain keys from the X12
content, not local filenames or arrival position.

The normal claim lifecycle is still easiest to read when the 837 arrives before
the 835: the first stitch creates a claim-only aggregate version and the later
835 stitch adds adjudication as the next version. If an 835 arrives first, the
stitcher can create or update the same claim aggregate from `CLP01`; a later
837 with the same claim id will update that aggregate when it is stitched.

Ordering does affect version history. Out-of-order arrival can produce an
intermediate remit-only or claim-only version before the aggregate becomes
complete. For audit rebuilds, replay the complete journal explicitly; for normal
operations, use incremental stitch with the read store so later drops can find
and update earlier aggregate state by key.
