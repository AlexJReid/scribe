# scribe

`scribe` turns healthcare X12 EDI into auditable events.

Instead of giving you a huge opaque JSON dump, it emits small domain events
with control numbers, segment positions, byte offsets, and run IDs so pipelines
can validate, load, replay, and debug claims/remits without hand-rolling brittle
string parsers.

Use it in three layers:

1. **Parse** EDI into NDJSON events.
2. **Ingest** charge rows and X12 files into an immutable journal.
3. **Project** the journal into claim, coverage, balance, and notification read models.

The parser handles 834 enrollment, 837 claims, 835 remits, and 270/271 eligibility
traffic. Raw PHI can be kept out of normal flows by writing tokenised events and
resolving sensitive values through a separate PHI vault only when required.

## Examples

Parse an 837 claim file and filter the emitted event stream:

```bash
scribe parse --type 837 claims.edi \
  | jq 'select(.event_type=="SubscriberReferenced")'
```

Ingest multiple inputs into a replayable evidence stream:

```bash
scribe ingest --out journal.scribe \
  --837 claims.edi \
  --835 remit.edi \
  --charges charges.ndjson
```

Project a balance from the journal:

```bash
scribe project balance \
  --journal journal.scribe \
  --encounter-id demo
```

`scribe` is a small C binary. Run it interactively, in shell pipelines, as a
serverless trigger when files arrive in S3, as a cron/batch job, as a K8S job, or
inside common data schedulers.

For more advanced usage see [scripts/stroke-demo.sh](./scripts/stroke-demo.sh),
[demo.sh](./demo.sh), or `scribe --help`.

## Build

No releases yet, but binaries are attached to each Action run.

The case study will build if not needed. On Linux and Windows you'll need sqlite3. See [CI](./github/workflows) for exact packages. macOS has it already.

```sh
./scripts/stroke-demo.sh
```

or

```sh
cmake -S . -B build
cmake --build build
```

## Shape

- Inputs: charge NDJSON, 834 enrollment, 837 claims, 835 remits, 270/271
  eligibility
- Events: small auditable facts with source transaction, control numbers,
  segment index, byte offset, and optional run ID
- Journal: immutable binary evidence stream
- PHI vault: raw PHI resolver, separate from normal stores
- Read store: indexes, versioned aggregate snapshots, and latest rows
- Outputs: claim aggregates, member coverage, balances, and outbox facts

SQLite is used as a stand-in for a managed database to back the vault and read
stores, but this can be swapped out in the future.

## Demo

The synthetic _stroke encounter_ case study lives in
[tests/fixtures/stroke_encounter/](./tests/fixtures/stroke_encounter/). Generated
reference output lives in [demo/](./demo).

```sh
./scripts/stroke-demo.sh
./demo.sh
```

Expected current encounter balance: `550.00`.

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

- [theory.md](./theory.md): compact model notes
- [events.md](./events.md): event names
- [tests/fixtures/stroke_encounter/README.md](./tests/fixtures/stroke_encounter/README.md): fixture map

## License

MIT. See `LICENSE`.
