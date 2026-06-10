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
service lines, billed amounts, dates, diagnoses, and locators.

835 says how the payer adjudicated it: status, payer control number, paid
amount, patient responsibility, adjustments, remittance dates, service-line
payments, and locators.

837 `CLM01` and 835 `CLP01` share the `claim_id` namespace. 835 `CLP07` uses
`payer_claim_control_number`.

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
