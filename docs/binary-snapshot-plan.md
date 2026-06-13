# Plan: Binary internal snapshots, JSON only as generated output

Status: proposal (not yet implemented)
Author: drafted 2026-06-13

## Problem

The durable aggregate state in the read store is a JSON string in a
`state_json TEXT` column. One hand-written serializer produces it, and a
matching hand-written hydrator parses it back with raw yyjson calls
(`yyjson_obj_get` / `yyjson_is_*` / `yyjson_get_*`). Two things are wrong:

1. **JSON is used as a storage format, not just an output format.** The read
   store pays JSON parse cost on every hydrate, incremental reduce, and
   projection, and the stored bytes are opaque to SQL anyway.
2. **The serialization library leaks into domain modules.** `snapshot.c`,
   `coverage_stitcher.c`, and `balance_projector.c` — code about claims and
   money — are full of `yyjson_val *` and parser AST walking on the read side.
   There is a `json_write` abstraction for output but no matching read
   abstraction, so every consume path open-codes the parser.

The motivating complaint is (2): yyjson peppered through domain logic. (1) is
the underlying cause.

## Decision

- **Internal durable snapshot → versioned binary blob** in SQLite
  (`state_blob BLOB`). The stitcher encodes the in-memory aggregate to binary
  and hydrates from binary. The raw-yyjson read paths are deleted.
- **External delta / outbox → stays JSON, generated on demand.** The delta
  exporter and outbox payloads decode the binary snapshot and run the existing
  `json_write` serializer to emit JSON at export time. JSON survives only as a
  transport/output format, never as storage.
- **Binary is the single source of truth.** No dual columns. (Decision:
  "re-serialize at export".)

This satisfies both the "JSON should not be storage" principle and the
"no raw yyjson in domain code" complaint: after this, `yyjson` appears only in
`src/json/` (output) and the export seam (output).

## What the JSON blob feeds today (so we don't break a consumer)

- `claim_aggregate_latest` / `claim_aggregate_versions` — internal state. Read
  by `claim_stitch_hydrate_snapshot` (incremental reduce) and by the projection
  via `scribe_store_each_latest_claim_aggregate`. **-> binary.**
- `member_coverage_latest` / `member_coverage_versions` — same shape for
  coverage. **-> binary.**
- Delta exporter (`delta_exporter.c`) copies `state_json` verbatim into the
  exported delta DB's `aggregate_versions` table. **-> decode binary, emit JSON.**
- Outbox `payload_json` — cursorable external facts. Already a distinct
  serialization; **stays JSON**, produced at write time from the live struct.

## On-disk format

A small, explicit, versioned binary encoding. No raw struct blitting — the
aggregate holds heap pointers (`service_lines`, `source_events.data`), so the
struct cannot be memcpy'd; fields are walked explicitly, same as the JSON
serializer does today.

Framing primitives (little-endian, length-prefixed):

- `u8`, `u32`, `u64`
- `string`: `u32 length` + bytes (no NUL; may be empty)
- `array<T>`: `u32 count` + `count` encoded elements
- `optional<T>`: `u8 present` + (T if present)

Blob layout:

```
magic   : u32  ('SCB1' or similar, sanity + endianness guard)
version : u16  format version, bumped on field changes
body    : encoded aggregate fields, in a fixed declared order
```

**Versioning is now our responsibility.** Field evolution must be append-only
within a `version`, and the decoder must handle every historical `version` it
may meet in a durable read store, or bump `version` and provide a migration.
JSON gave forward/backward tolerance for free; binary does not. This is the
single biggest ongoing cost of the change and must be disciplined.

## New / changed surface

New:

- `src/util/blob_writer.{h,c}` — append-only byte buffer with the framing
  primitives above (grow-on-demand, mirrors `json_writer` ergonomics).
- `src/util/blob_reader.{h,c}` — cursor over a blob with matching readers and
  bounds checking (every read validates remaining length; a truncated or
  malformed blob is a clean error, never an over-read).
- Per-type codecs: claim aggregate encode/decode, coverage encode/decode.
  These replace `build_snapshot_state_json` + `*_hydrate_snapshot` on the
  *storage* path. The field walk stays; its target changes from yyjson to blob.

Changed:

- `sqlite_store.c`: `state_json TEXT` -> `state_blob BLOB` on the four
  `_latest` / `_versions` tables; `bind_blob` / `column_blob` helpers; the
  store API changes from `const char *state_json` / `char *out, size_t len` to
  a blob pointer + length (in and out), because blobs are not NUL-terminated
  and may contain zero bytes.
- `aggregate_stitcher.c` / coverage stitcher: hydrate from binary; drop
  `STITCH_STATE_JSON_MAX` / `COVERAGE_STATE_JSON_MAX` scratch buffers.
- `delta_exporter.c`: decode binary -> emit JSON for the exported delta.
- `balance_projector.c`: consume via binary decode instead of yyjson.
- `tests/test_store.c`: it currently puts a JSON string and asserts
  `strstr(state_json, "\"version\":3")`. Becomes a binary round-trip assertion
  (decode and check the version field), since the column is no longer text.
- `tests/test_parser.c`: assertions that pin the on-disk JSON shape move to the
  *export* artifacts (delta / outbox / projection JSON), which are still JSON.

## Staging (suite green at every stage)

1. **Codec core.** `blob_writer` / `blob_reader` + a unit test round-tripping
   every primitive, including truncation/overflow rejection. No wiring.
   **Pause here for format review — this is where the format is locked in.**
2. **Claims storage.** Claim encode/decode; switch claim `_latest`/`_versions`
   to `BLOB`; store API to blob; delete claim read-side yyjson; migrate
   `test_store.c`. Claim tests green.
3. **Coverage storage.** Same for coverage.
4. **Export seam.** Binary -> JSON in delta exporter and outbox; projector reads
   binary. Move JSON-shape test assertions to export artifacts. Full suite.
5. **Cleanup.** Remove dead JSON-read helpers and `*_STATE_JSON_MAX`; update
   `model.md` (storage is binary; JSON is output) and `caveats.md` (format
   versioning is now a maintained contract). Optional: a `scribe dump-snapshot`
   debug command to restore human-inspectability.

## Honest assessment

**Is this worth doing? Partially, and smaller than it looks like it should be.**

What it genuinely fixes:
- Removes raw yyjson from the read side of domain code — the actual complaint.
  After this, parsing a stored aggregate is one `decode()` call, not 30+ AST
  pokes per type.
- Makes JSON honestly an output format. The principle holds end to end.
- Removes the JSON parse cost on the hot internal paths (hydrate / reduce /
  project) and the fixed `*_STATE_JSON_MAX` scratch caps.

What it costs, and where I'd push back:
- **It does not reduce the hand-written field-walk surface.** The schema has to
  live somewhere; today it's the serialize/hydrate pair, after this it's the
  encode/decode pair. We trade "yyjson field walk" for "blob field walk." The
  read side gets cleaner (no AST, no `is_*` type checks), but the maintenance
  shape — two functions per type that must stay in lockstep — is unchanged.
  A *declarative field table* driving both directions would actually shrink
  that surface; binary-vs-JSON does not.
- **We take on a durability/versioning burden JSON gave us free.** A careless
  field reorder silently corrupts existing read stores. This is a permanent tax
  on every future schema change to an aggregate.
- **We lose human-inspectable storage.** `sqlite3 ... 'select state_json'` no
  longer tells you anything; debugging needs a decode tool.
- **It does not fix queryability** (still opaque blobs). The user has said this
  is out of scope, which is fine, but it means the read store stays
  scan-and-deserialize for any predicate.

**My recommendation.** If the real goal is "get yyjson out of domain code,"
there is a smaller change that achieves most of it without the durability tax:
introduce a **`json_read` reader abstraction** (the missing mirror of
`json_write`) and refactor the three modules to it. yyjson then lives only in
`src/json/`, the domain code stops touching the AST, and the storage format and
its forward-compatibility stay unchanged. That is lower risk and lands in a
fraction of the time.

Go binary when the driver is *also* parse cost or a deliberate move to make the
read store a true internal format — not purely the aesthetic of "no JSON in
storage." The binary plan above is the right design *if* that is the goal; it is
heavier than the complaint strictly requires.

Net: the binary plan is sound and I will execute it stage by stage if chosen,
pausing after Stage 1. But honestly, a `json_read` abstraction is the better
effort-to-value trade for the stated motivation, and binary is the right call
mainly if you also want the read store to stop being JSON on principle and are
willing to own format versioning forever.
