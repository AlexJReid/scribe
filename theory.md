# scribe notes

`scribe` joins X12 files that normally arrive apart:

- 834 enrollment context
- 270/271 eligibility inquiry and response
- 837 claim submissions
- 835 remittance advice

The normal path is tokenised. Raw PHI goes to a separate vault and should only
be resolved for controlled PHI workflows.

## Claims

```text
837 claim + 835 remittance
    -> journal events
    -> claim aggregate versions
```

837 says what was claimed: identity, patient/subscriber/provider references,
service/revenue lines, billed amounts, dates, diagnoses, and locators.

`ClaimServiceLineRecorded` carries the submitted line facts needed for matching
and projection: line order, procedure qualifier/code, procedure modifiers,
charge amount, unit measure, unit count, and diagnosis pointers where present.
Institutional `SV2` lines also carry the revenue code. Consumers should use
these named fields, not infer submitted amounts and units from `raw_elements`.

Provider references keep their role in the event name. Current 837 provider
roles include billing, rendering, referring, supervising, facility, attending,
operating, and other provider references. `reference_scope` says whether the
provider applied at claim or service-line scope.

835 says how the payer adjudicated it: status, payer control number, paid
amount, patient responsibility, adjustments, remittance dates, service-line
payments, and locators.

837 `CLM01` and 835 `CLP01` share the `claim_id` namespace. 835 `CLP07` uses
`payer_claim_control_number`.

The `x12_005010x222_example_01_synthetic.edi` fixture is a synthetic,
non-verbatim 837P stream shaped after X12's public 005010X222 Example 01. Keep
official X12 example text out of the repo unless licensing is explicit.

## Coverage

```text
834 enrollment + 270 inquiry + 271 response
    -> journal events
    -> member_coverage aggregate versions
```

Coverage stays separate from claim aggregates. It is member-centric and
temporal: 834 supplies roster truth, 270 records the question, and 271 records
the payer answer. Claim workflows can consult it by member token, payer token,
service date, and service type.

## Storage

- Journal: immutable binary evidence stream, normally stored in arrival-date/run
  segments.
- PHI vault: `namespace + token -> raw`.
- Event indexes: claim, payer control, member, payer, service type,
  and journal locator lookup.
- Snapshot store: versioned claim/member coverage states plus latest rows.
- Outbox: non-PHI `AggregateVersionRecorded` facts.

SQLite backs these stores in the proof of concept.

### SQLite Zones

The journal is the evidence store; SQLite is the index and materialization
surface around it. Keep the zones conceptually separate even when the proof of
concept stores several of them in one SQLite file.

| Zone | Current tables | What it is for | Why it is needed |
| --- | --- | --- | --- |
| Source drop catalog | `source_drops` | One row per inbound source drop or X12 transaction batch, with type, source file, received time, and hash/provenance fields. | Gives stable provenance for PHI mappings, aggregate versions, and replay/debug without treating local journal filenames as business identity. |
| Journal locator index | `events` | Stable event id to source drop, event type, journal segment id/path, byte offset, stored length, and checksum. | Lets processors and auditors seek back to the exact journal bytes that produced a fact instead of rescanning huge journal partitions. |
| Event key index | `event_keys` | Secondary keys extracted from individual journal events, such as claim id, payer control number, member id, payer id, eligibility id, and service type. | Supports lookup by domain key and lets the shuffler decide which aggregates a new source drop may affect. This is an index over facts, not aggregate state. |
| Aggregate key index | `claim_aggregate_keys`, `member_coverage_keys` | Durable mapping from domain keys to aggregate ids. Claim examples: claim id, payer claim control number. Coverage examples: member id, payer id, service type/date keys. | Lets a later drop, such as an 835, find the aggregate originally created by an earlier 837 without replaying the whole journal. |
| Dirty routing | `aggregate_event_routes`, `dirty_aggregates` | Event-to-aggregate routing plus pending dirty aggregate marks for a source drop. | Separates pass 1 shuffle from pass 2 reduce: ingest/index the new segment, mark affected aggregates dirty, then hydrate and reduce only those aggregate states. |
| Aggregate versions | `claim_aggregate_versions`, `member_coverage_versions` | Append-only version history of materialized aggregate snapshots, including updated-by event and source drop. | Preserves the state transition history and gives consumers idempotent `(aggregate_id, version)` facts. |
| Latest aggregate cache | `claim_aggregate_latest`, `member_coverage_latest` | Current snapshot for each aggregate id. | Lets incremental reducers start from latest state and apply only new routed events, making normal processing proportional to the new drop. |
| PHI vault | separate PHI SQLite, currently `phi_mappings` and audit rows | Token-to-raw resolver scoped by namespace, actor, and purpose. | Keeps normal read stores tokenised/non-PHI. Controlled PHI workflows resolve values explicitly and can be audited separately. |
| Outbox | currently NDJSON `AggregateVersionRecorded` output, future table | Delivery facts emitted after aggregate versions are recorded. | Keeps subscriber delivery/retry/cursor concerns out of the aggregate reducer. |

The key distinction is that `event_keys` answer "which journal events mention
this key?" while aggregate key indexes answer "which durable aggregate owns this
key now?" Dirty routing ties the two together for incremental work.

### Incremental Shuffle/Reduce

Normal processing should not replay the entire journal. The steady-state path is
two pass and source-drop driven:

```text
new journal segment
  -> shuffle/index pass
  -> dirty aggregate ids
  -> reduce dirty aggregate ids from latest snapshots
  -> new aggregate versions + outbox facts
```

Pass 1, shuffle/index:

- Read only the new `.journal` segment.
- Assign stable read-store event ids from segment path, byte offset, and stored
  length.
- Record source drop metadata and journal locators.
- Extract event keys such as claim id, payer claim control number, member id,
  payer id, eligibility id, and service type.
- Use aggregate key indexes to route each event to one or more durable aggregate
  ids.
- Mark those aggregate ids dirty for the source drop.

Pass 2, reduce:

- For each dirty aggregate id, load the latest aggregate snapshot.
- Hydrate only that aggregate state.
- Apply only new events routed from the current source drop.
- Persist the next aggregate version and update aggregate key indexes.
- Clear the dirty mark after the new version is recorded.

This means an 835 drop can find and update a claim aggregate created by an
earlier 837 through `claim_aggregate_keys`, without scanning older 837 journal
segments. Coverage follows the same pattern through `member_coverage_keys`.

Full replay remains useful for rebuilds and audits, but it should be explicit.
The normal append path should be proportional to the new source drop and the
aggregates it touches.

## Stable IDs

Treat the binary journal as the evidence store and keep durable identity out of
arrival order or local filenames.

- Journal segment: arrival date plus ingest run file.
- Source drop: stable inbound X12 transaction identity, `type:isa13:gs06:st02`
  when ISA/GS/ST controls are present.
- Event locator: journal segment plus byte offset and stored length.
- Aggregate partition: domain key, normally claim token for claims and member
  token plus payer/service context for coverage.

Source drop IDs identify the file/interchange/transaction batch that should
collapse into one aggregate version. Event locators identify the exact bytes that
produced a fact, so renamed files or reordered ingest do not change evidence
references.

Worked example, as used by the stroke demo:

```sh
mkdir -p demo/stroke.journal.d/20260617 demo/stroke.journal.d/20260720

scribe ingest --out demo/stroke.journal.d/20260617/stroke-drop-facility-837.journal \
  --run-id stroke-drop-facility-837 \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --837 tests/fixtures/stroke_encounter/facility_837.edi

scribe ingest --out demo/stroke.journal.d/20260720/stroke-drop-facility-835.journal \
  --run-id stroke-drop-facility-835 \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --835 tests/fixtures/stroke_encounter/facility_835.edi

scribe stitch claims --journal demo/stroke.journal.d \
  --read-store demo/stroke_read_store.sqlite \
  --out demo/stroke_aggregates.ndjson
```

Each ingest writes one binary journal segment under an arrival-date partition.
The stitch/project readers accept either one segment file or a directory tree of
`.journal` segment files and replay them in lexical path order. The 837 and 835
files keep distinct source drop IDs from their X12 controls, while `run_id`
identifies the ingest execution that wrote that segment.

PHI vault mappings store first/last source drop IDs, not file paths. To resolve
that provenance back to an inbound file, join the source drop ID through the read
store `source_drops` metadata, which records the source type and source file.

## Runs

Each execution should have a `run_id`.

- Journal event `run_id`: ingest execution.
- Aggregate/notification `run_id`: stitch execution.
- `source_run_id`: ingest run copied from reduced journal events.
- `source_drop_id`: stable inbound X12 transaction identity when controls are
  available, formatted as `type:isa13:gs06:st02`.
- `updated_by_*`: last journal event and locator that changed the batch.

The stitcher records new aggregate versions. A notifier owns delivery retries,
subscriber cursors, and dead letters. Consumers should use `(aggregate_id,
version)` for idempotency.

## PHI

Default path omits names, tokenises identifiers, keys aggregates by tokens, and
keeps free text out of normal stores unless tokenised.

```text
secret + namespace + raw value -> token
namespace + token -> raw value
```

Tokens use HMAC-SHA256 with `SCRIBE_TOKEN_KEY`, keyed by
`namespace + ":" + raw value`, truncated to 16 digest bytes as 32 lowercase hex
characters.

Hashing is not encryption. PHI read stores need an owner, purpose, expiry,
retention rule, encrypted storage, and audited access.

## Balance

The synthetic stroke fixture has radiology facility, radiology professional,
rehab, and neurology follow-up claims. Expected totals: billed `3720.00`, paid
`2340.00`, patient responsibility `550.00`.
