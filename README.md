<img src="./assets/scribe-logo-v2.png" width=200>

# Scribe

`scribe` turns healthcare X12 EDI into auditable events.

Instead of giving you a huge opaque JSON dump, it emits small domain events
with control numbers, segment positions, byte offsets, and run IDs so pipelines
can validate, load, replay, and debug claims/remits without hand-rolling brittle
string parsers.

Use it in five layers:

1. **Parse** EDI into JSON events to understand/debug content.
2. **Ingest** X12 files into an immutable journal.
3. **Stitch** journal events into claim and coverage aggregate versions, matching
   837 claim submissions with 835 remittances.
4. **Project** the journal into balance and notification read models.
5. **Consume** applications and reports read directly from the projected read store(s).

The parser handles 834 enrollment, 837 claims, 835 remits, and 270/271 eligibility
traffic. Raw PHI can be kept out of normal flows by writing tokenised events and
resolving sensitive values through a separate PHI vault only when required.

> scribe parses X12 syntax and maps selected healthcare EDI facts into journal events. It is not (yet) a full X12/TR3 validator.

## Examples

Parse an 837 claim file and filter the emitted event stream:

```bash
scribe parse --type 837 claims.edi \
  | jq 'select(.event_type=="ClaimReferencedSubscriber")'
```

Ingest multiple inputs into a replayable evidence stream:

```bash
scribe ingest --out journal.scribe \
  --837 claims.edi \
  --835 remit.edi
```

Write source drops into partitioned journal segments:

```bash
scribe ingest --out journal.d/20260617/drop-001.journal --run-id drop-001 --837 claims.edi
scribe stitch claims --journal journal.d/20260617/drop-001.journal \
  --incremental --read-store read_store.sqlite --out changed_claims.ndjson

scribe ingest --out journal.d/20260720/drop-002.journal --run-id drop-002 --835 remit.edi
scribe stitch claims --journal journal.d/20260720/drop-002.journal \
  --incremental --read-store read_store.sqlite --out changed_claims.ndjson
```

Stitch claim versions by matching 837 claim facts with 835 remittance facts,
then populate read-store indexes:

```bash
scribe stitch claims \
  --journal journal.scribe \
  --read-store read_store.sqlite \
  --out claim_aggregates.ndjson \
  --notify-out notifications.ndjson
```

The stitch `--out` NDJSON is a debug/inspection stream of changed aggregate
versions from that run. It is useful for tests, demos, diffs, and operational
logs, but it is not the durable application interface. Consumers should read the
versioned and latest aggregate rows from the read store. `--notify-out` is
different: it is the outbox handoff for aggregate-version notifications. In this
proof of concept that handoff is NDJSON, while a deployment would usually write
the same delivery facts to a managed outbox table for a separate fan-out service
to deliver to subscribers.

`--incremental` treats `--journal` as the new source-drop segment: the stitcher
indexes that segment, hydrates only touched aggregates from the read store, and
emits changed versions. Omit `--incremental` for an explicit full replay/rebuild.

When X12 control numbers are present, `source_drop_id` uses
`txn:ISA13:GS06:ST02`; for example, `271:000000111:111:0001` is a 271
source drop with ISA13 `000000111`, GS06 `111`, and ST02 `0001`.

Claims match on tokenised `CLM01`/`CLP01`. Service lines are paired by procedure
and charge, date when available, or line order, with the chosen `match_method`
included in the aggregate output.

Claim aggregates keep the selected 837 facts needed by readers without carrying
the whole EDI document: claim envelope, subscriber/patient context, claim-level
dates, claim/service references, diagnosis summary, healthcare-code components,
and service lines. Other mapped facts remain journal evidence until a projection
needs them.

837 claim events expose the core `CLM` envelope directly: total charge, CLM05
facility type/qualifier/frequency, provider signature, assignment,
benefits-assignment, release-of-information, and patient-signature indicators.
Subscriber/patient context from `SBR`, `PAT`, and `DMG` is emitted as
claim-scoped events after the claim ID is known; DOB and identifier-like values
stay tokenised by default.

837 service-line events expose submitted procedure, modifiers, charge, unit
measure, unit count, and diagnosis pointer arrays directly. Professional `SV1`
lines and institutional `SV2` revenue lines use the same
`ClaimServiceLineRecorded` event; `SV2` also carries `revenue_code`.
Claim/service scoped `REF` segments become `ClaimReferenceRecorded` with
tokenised reference identifiers. `raw_elements` remains available as source
evidence on service-line and diagnosis summaries, but stitch/projection code
reads the named fields.

837 `HI` emits both a diagnosis summary and per-component healthcare-code facts.
That covers the common 837I condition, occurrence, value, and procedure-style
components as typed journal evidence without turning the mapper into a full TR3
validator. `CL1` institutional claim information records admission type,
admission source, and patient status, while claim-level `DTP` admission and
discharge dates are retained in aggregate state.

837 provider references include billing, rendering, referring, supervising,
facility, attending, operating, and other provider roles, with
`reference_scope` indicating claim or service-line scope. Provider `PRV`
segments emit taxonomy/specialty facts through `ClaimProviderTaxonomyRecorded`.

`scribe` is a small C binary. Run it interactively, in shell pipelines, as a
serverless trigger when files arrive in S3, as a cron/batch job, as a K8S job, or
inside common schedulers.

For more advanced usage see [scripts/stroke-demo.sh](./scripts/stroke-demo.sh),
[demo.sh](./demo.sh), or `scribe --help`.

## Build

Release binaries are attached to
[the latest GitHub release](https://github.com/AlexJReid/scribe/releases/latest).

The case study will build if needed. On Linux and Windows you'll need sqlite3. See [CI](./.github/workflows) for exact packages. macOS has it already.

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
  directory of `.journal` segment files
- PHI vault: raw PHI resolver, separate from normal stores
- Read store: indexes, versioned aggregate snapshots, and latest rows
- Debug/output streams: aggregate/version NDJSON and balances.
  Stitch aggregate NDJSON is for inspection; applications should consume the
  read store.
- Outbox stream: aggregate-version notification facts for a separate fan-out
  service.

SQLite is used as a stand-in for a managed database to back the vault and read
stores, but this can be swapped out in the future.

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

All PHI-looking fixture values are made up. The stroke case study is only
inspired by a UK, non-US healthcare episode that I personally had. IDs, payer
details, dates, amounts, and EDI content are made up.

## More

- [model.md](./model.md): compact model notes
- [FAQ.md](./FAQ.md): matching, stitching, deployment, and ordering notes
- [events.md](./events.md): event names
- [tests/fixtures/stroke_encounter/README.md](./tests/fixtures/stroke_encounter/README.md): fixture map

## License

MIT. See `LICENSE`.
