# scribe

Proof of concept for stitching provider charge context, 837 claims, and 835
remittances into versioned claim aggregates and ledger-style balance projections.

## What

`scribe` is about joining the parts of a healthcare money trail that usually
arrive as separate files:

- provider charges: local context about work performed
- 834 enrollment: member and coverage change context
- 270/271 eligibility: coverage questions and payer answers at a point in time
- 837 claims: provider-to-payer claim submissions
- 835 remittances: payer-to-provider adjudication and payment detail

The proof of concept turns those inputs into an immutable journal. The claim
money trail reduces into versioned claim aggregates and balance projections;
coverage/member evidence reduces into temporal `member_coverage` aggregates.
PHI-bearing values are tokenized before they enter the normal journal/read-store
path, with raw values held separately in a PHI vault.

[Read more](./theory.md)

## Case study

The main case study is a semi-synthetic stroke recovery encounter:

- Encounter: `ENC-SYN-STROKE-001`
- Patient: synthetic `ALEX REID`
- Story: CT without contrast, CT with contrast, MRI, rehab, neurology follow-up
- Fixtures: `tests/fixtures/stroke_encounter/`
- Walked output: `demo/`

All PHI-looking values are synthesized. The case study is inspired by the broad
shape of a stroke-related episode I had in the UK, outside the US healthcare
system. Names, IDs, payer details, dates, amounts, and EDI content are invented
for this proof of concept and are not real PHI.

## Architecture Decisions

- Use an immutable binary journal as the evidence stream, derived from .edi
- Split PHI early: tokenised events are the default path; raw values stay in a
  separate PHI vault
- Store event locators, not payloads, in the read-store event index
- Materialise versioned claim aggregates plus a latest snapshot for consumers
- Emit lightweight notifications (no PHI payload) when an new snapshot is generated for downstream to consume
- Use SQLite as a stand-in for the PHI vault, index, and snapshot store: a managed document DB would replace this

More background on the 837/835 model, tokenisation, and PHI tradeoffs lives in
[theory.md](theory.md).

## Modeling

- `member_coverage` aggregate: stitches 834 enrollment changes with 270
  eligibility inquiries and 271 eligibility responses into member-centric,
  temporal coverage context. It stays separate from claim aggregates; claim
  workflows can reference it by member token, payer token, service date, and
  service type.

## Build

Tested on macOS; Linux should be fine. MSVC should be possible with project-file
work.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Stroke Demo

`demo/` contains generated output from the walked stroke case study:

- `demo/stroke.journal`
- `demo/stroke_phi_vault.sqlite`
- `demo/stroke_read_store.sqlite`
- `demo/stroke_aggregates.ndjson`
- `demo/stroke_member_coverage.ndjson`
- `demo/stroke_phi_member_coverage.ndjson`
- `demo/stroke_notifications.ndjson`

Run the walked demo:

```sh
./demo.sh
```

If `demo/` is missing or stale, regenerate it first:

```sh
./scripts/stroke-demo.sh
```

Or regenerate the journal and PHI vault directly:

```sh
build/scribe journal --out demo/stroke.journal \
  --run-id stroke-ingest-demo \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --charges tests/fixtures/stroke_encounter/charge_transactions.ndjson \
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
```

Stitch into the tokenised read store:

```sh
build/scribe stitch \
  --journal demo/stroke.journal \
  --encounter-id ENC-SYN-STROKE-001 \
  --read-store demo/stroke_read_store.sqlite \
  --notify-out demo/stroke_notifications.ndjson \
  --run-id stroke-stitch-demo \
  --out demo/stroke_aggregates.ndjson
```

`stroke_notifications.ndjson` contains non-PHI `AggregateVersionRecorded`
records for downstream delivery. Treat it as a derived JSON outbox/debug hook:
a notifier can scan it from its last offset and use `(aggregate_id, version)`
for idempotency. The binary journal remains the source evidence.

Reduce coverage/member context:

```sh
build/scribe coverage \
  --journal demo/stroke.journal \
  --read-store demo/stroke_read_store.sqlite \
  --run-id stroke-coverage-demo \
  --out demo/stroke_member_coverage.ndjson
```

Create a PHI-containing read store only when needed:

```sh
build/scribe stitch \
  --journal demo/stroke.journal \
  --encounter-id ENC-SYN-STROKE-001 \
  --read-store demo/stroke_phi_read_store.sqlite \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --include-phi \
  --out demo/stroke_phi_aggregates.ndjson
```

The coverage reducer follows the same PHI switch:

```sh
build/scribe coverage \
  --journal demo/stroke.journal \
  --read-store demo/stroke_phi_read_store.sqlite \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --include-phi \
  --out demo/stroke_phi_member_coverage.ndjson
```

Project the ledger-style balance:

```sh
build/scribe project --projection balance \
  --journal demo/stroke.journal \
  --encounter-id ENC-SYN-STROKE-001 \
  --out demo/stroke_balance.json
```

Expected current balance: `550.00`.

## Inspect

```sh
sqlite3 -header -column demo/stroke_read_store.sqlite "
select aggregate_id, version, state_json
from claim_aggregate_latest
order by aggregate_id;
"
```
The read store tables are `event_keys`, `events`, `claim_aggregate_versions`,
`claim_aggregate_latest`, `member_coverage_versions`,
`member_coverage_latest`, and `member_coverage_keys`.

## License

MIT. See `LICENSE`.
