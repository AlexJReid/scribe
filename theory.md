# scribe notes

`scribe` joins files that normally arrive apart:

- charge rows from a local provider system
- 834 enrollment context
- 270/271 eligibility inquiry and response
- 837 claim submissions
- 835 remittance advice

The normal path is tokenised. Raw PHI goes to a separate vault and should only
be resolved for controlled PHI workflows.

## Claims

```text
charges + 837 claim + 835 remittance
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

- Journal: immutable binary evidence stream.
- PHI vault: `namespace + token -> raw`.
- Event indexes: claim, payer control, encounter, member, payer, service type,
  and journal locator lookup.
- Snapshot store: versioned claim/member coverage states plus latest rows.
- Outbox: non-PHI `AggregateVersionRecorded` facts.

SQLite backs these stores in the proof of concept.

## Runs

Each execution should have a `run_id`.

- Journal event `run_id`: ingest execution.
- Aggregate/notification `run_id`: stitch execution.
- `source_run_id`: ingest run copied from reduced journal events.
- `source_drop_id`: source file or transaction group reduced as one batch.
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
