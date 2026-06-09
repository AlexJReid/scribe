# scribe

`scribe` is a C proof of concept for parsing synthetic healthcare EDI into a
tokenised, replayable money trail.

It ingests charge rows plus 834, 835, 837, and 270/271 files; writes an
immutable binary journal; keeps raw PHI in a separate vault; and reduces the
journal into claim aggregates, coverage snapshots, balance projections, and
non-PHI notification facts.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run focused checks with `build/scribe ...`.

## Shape

- Inputs: charge NDJSON, 834 enrollment, 837 claims, 835 remits, 270/271
  eligibility.
- Journal: immutable binary evidence stream.
- PHI vault: raw PHI resolver, separate from normal stores.
- Read store: indexes, versioned aggregate snapshots, and latest rows.
- Outputs: claim aggregates, member coverage, balances, and outbox facts.

SQLite backs the vault and read stores in this proof of concept.

## Demo

The walked synthetic stroke case lives in `tests/fixtures/stroke_encounter/`;
generated reference output lives in `demo/`.

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

See `scripts/stroke-demo.sh` for the full ingest, stitch, coverage, PHI, and
balance command lines.

## Safety

Default flows stay tokenised. Use `--include-phi --phi-vault ...` only for
controlled PHI read stores.

All PHI-looking fixture values are synthesized. The stroke case study is only
inspired by a UK, non-US healthcare episode; names, IDs, payer details, dates,
amounts, and EDI content are invented.

## More

- `theory.md`: compact model notes.
- `EVENTS.md`: event names.
- `tests/fixtures/stroke_encounter/README.md`: fixture map.

## License

MIT. See `LICENSE`.
