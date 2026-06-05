# scribe

Proof of concept for stitching provider charge context, 837 claims, and 835
remittances into versioned claim aggregates and ledger-style balance projections.

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

- Use an immutable binary journal as the evidence stream.
- Split PHI early: tokenised events are the default path; raw values stay in a
  separate PHI vault.
- Store event locators, not payloads, in the read-store event index.
- Materialise versioned claim aggregates plus a latest snapshot for consumers.
- Use SQLite as the proof-of-concept vault, index, and snapshot store.
- Keep this as a small C executable, not a service.

More background on the 837/835 model, tokenisation, and PHI tradeoffs lives in
[theory.md](theory.md).

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

Regenerate the journal and PHI vault:

```sh
build/scribe journal --out demo/stroke.journal \
  --phi-vault demo/stroke_phi_vault.sqlite \
  --charges tests/fixtures/stroke_encounter/charge_transactions.ndjson \
  --837 tests/fixtures/stroke_encounter/facility_837.edi \
  --837 tests/fixtures/stroke_encounter/professional_837.edi \
  --837 tests/fixtures/stroke_encounter/rehab_837.edi \
  --837 tests/fixtures/stroke_encounter/neurology_837.edi \
  --835 tests/fixtures/stroke_encounter/facility_835.edi \
  --835 tests/fixtures/stroke_encounter/professional_835.edi \
  --835 tests/fixtures/stroke_encounter/rehab_835.edi \
  --835 tests/fixtures/stroke_encounter/neurology_835.edi
```

Stitch into the tokenised read store:

```sh
build/scribe stitch \
  --journal demo/stroke.journal \
  --encounter-id ENC-SYN-STROKE-001 \
  --read-store demo/stroke_read_store.sqlite \
  --out demo/stroke_aggregates.ndjson
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
The read store tables are `event_keys`, `events`, `claim_aggregate_versions`, and
`claim_aggregate_latest`.

## License

MIT. See `LICENSE`.
