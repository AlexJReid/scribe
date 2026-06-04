# scribe

Proof of concept for stitching provider charge context, 837 claims, and 835
remittances into versioned claim aggregates and ledger-style balance projections.

The main case study is a semi-synthetic stroke recovery encounter:

- Encounter: `ENC-SYN-STROKE-001`
- Patient: synthetic `ALEX REID`
- Story: CT without contrast, CT with contrast, MRI, rehab, neurology follow-up
- Fixtures:
  [tests/fixtures/stroke_encounter](https://github.com/AlexJReid/scribe/tree/main/tests/fixtures/stroke_encounter)

The PHI-looking values are fake synthetic data. The treatment shape reflects
stroke-related treatment I had in the UK; names, IDs, payer details, dates,
amounts, and EDI content are not real PHI.

## Model

Current proof of concept:

- Journal: immutable evidence stream, NDJSON for now, binary log target
- PHI vault: separate resolver for `namespace + token -> raw`
- Indexes: claim, payer control, encounter, and event locator lookup
- Aggregate snapshot store: versioned claim state plus latest claim state

SQLite backs the vault, indexes, and snapshots in this proof of concept. It is
standing in for a managed database or document store.

### Good things about this approach

- Immutable journal for parsed 837/835 inputs
  - Events carry source file location references
- Early PHI split
  - Tokenised events can move through normal dev and analytics paths
  - Raw values stay in the vault
- Journal reductions can answer state as of T
  - Chronologies and historic claim timelines become simple projections
- Pre-calculated claim snapshots are one read for consuming apps
  - Snapshots can be tokenised or PHI-containing
  - New versions can kick subscribers rather than forcing one-off downloads
  - Delivery can be a topic, webhook, or consumer cursor for nightly catch-up
- PHI can be modelled as a stream or separate read store
  - Keeps raw values off the main journal while authorised HITRUST apps can join PHI locally at ingest
  - Or keep an audited API as the central vault for access control and lookup history
- `scribe` renders 835/837 JSON, journals, and aggregates for exploration
- SQLite is a flexible stand-in for read stores and vaults

**Figure 1: ingest writes journal evidence and PHI vault mappings.**

```mermaid
flowchart LR
    input["charges + 837 + 835"]
    ingest["ingest/parser<br/>map + tokenise"]
    journal[("journal<br/>NDJSON POC / binary target")]
    vault[("PHI vault<br/>namespace + token -> raw")]

    input --> ingest
    ingest --> journal
    ingest -. raw PHI mappings .-> vault
```

**Figure 2: the read store indexes events and materialises aggregates.**

```mermaid
flowchart LR
    journal[("journal")]
    store[("read store<br/>SQLite POC")]
    keys["event_keys"]
    events["events<br/>locator only"]
    versions["claim_aggregate_versions"]
    latest["claim_aggregate_latest"]
    notify["AggregateVersionRecorded<br/>topic / webhook / cursor"]
    subscribers["subscribed systems<br/>balance / timelines / work queues / analytics"]

    journal --> store
    store --> keys
    store --> events
    store --> versions
    versions --> latest
    latest --> notify
    notify --> subscribers
```

## Stroke demo

Create the journal and PHI vault:

```sh
build/scribe journal --out stroke.journal.ndjson \
  --phi-vault stroke_phi_vault.sqlite \
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

Stitch into the read store:

```sh
build/scribe stitch \
  --journal stroke.journal.ndjson \
  --encounter-id ENC-SYN-STROKE-001 \
  --read-store stroke_read_store.sqlite \
  --out stroke_aggregates.ndjson
```

`stroke_read_store.sqlite` contains:

- `event_keys`
- `events`
- `claim_aggregate_versions`
- `claim_aggregate_latest`

`stroke_aggregates.ndjson` is an inspection/export stream only.

Optional: derive a ledger-style balance from the same journal:

```sh
build/scribe project --projection balance \
  --journal stroke.journal.ndjson \
  --encounter-id ENC-SYN-STROKE-001 \
  --out stroke_balance.json
```

Expected current balance: `550.00`.

## Read store

Aggregates are in SQLite:

```sh
sqlite3 -header -column stroke_read_store.sqlite "
select aggregate_id, version, state_json
from claim_aggregate_latest
order by aggregate_id;
"
```

Version history:

```sh
sqlite3 -header -column stroke_read_store.sqlite "
select version, updated_by_event_id, source_drop_id, state_json
from claim_aggregate_versions
where aggregate_id = 'claim:8259c238232f9585e95fc8f45b0bb410'
order by version;
"
```

Journal locator lookup:

```sh
sqlite3 -header -column stroke_read_store.sqlite "
select ek.event_id, e.source_drop_id, e.event_type, e.segment_id,
       e.event_offset, e.event_length
from event_keys ek
join events e on e.event_id = ek.event_id
where ek.key_type = 'payer_claim_control_number'
  and ek.key_value = 'edf29f09740ab104da309e2b036e14d1';
"
```

`events` stores locators only, never payload or aggregate state:

```text
event_id, source_drop_id, event_type, segment_id, event_offset, event_length, checksum
```

## PHI

Default path is non-PHI:

- names omitted
- claim/control IDs tokenised
- aggregates keyed by tokens
- 837 `CLM01` and 835 `CLP01` share the `claim_id` namespace
- 835 `CLP07` uses `payer_claim_control_number`

```text
secret + namespace + raw value -> token
```

HMAC-SHA256 is used.

`SCRIBE_TOKEN_KEY` supplies the secret. Raw lookup goes through the vault:

```text
namespace + token -> raw value
```

HITRUST-zone apps may deliberately create/read PHI-containing aggregates with
`--include-phi --read-store`, or render PHI by resolving tokens through the
vault. Normal developer stores should stay tokenised.

## PHI aggregate rendering

Synthetic PHI view; treatment pattern reflects stroke-related care I had
in the UK.

```text
Encounter: ENC-SYN-STROKE-001
Patient:   ALEX REID

Claim                         Type                    Billed   Paid    PR
CLM-STROKE-RAD-FAC-001        radiology_facility      2350.00  1450.00 350.00
CLM-STROKE-RAD-PRO-001        radiology_professional   390.00   260.00  40.00
CLM-STROKE-REHAB-001          outpatient_rehab         660.00   420.00 120.00
CLM-STROKE-NEURO-001          neurology_followup       320.00   210.00  40.00

Totals: billed 3720.00, paid 2340.00, PR/current balance 550.00
```

## Build

Tested on macOS; likely fine on Linux. MSVC should be possible with project-file
work.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
