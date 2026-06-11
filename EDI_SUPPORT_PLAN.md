# Full EDI support roadmap

## Goal

Evolve scribe from a healthcare-focused X12 event extraction tool into a robust EDI processing platform while preserving its Unix philosophy:

- Read files or streams.
- Emit auditable NDJSON events.
- Compose with jq, DuckDB, Spark, Lambda, cron, S3 triggers, and message queues.
- Avoid becoming a heavyweight integration server.

Reference sources include CMS documentation, companion guides, and Stedi healthcare documentation. These are used to prioritise transaction coverage and validate behaviour, not as implementation dependencies.

## Phase 1 - complete 837 claim coverage

The immediate priority is to make 837 support useful enough to drive claim stitching, 835 matching, and downstream projections. That means supporting both professional and institutional claim shapes.

### Current progress

As of the current implementation pass:

- `ClaimServiceLineRecorded` emits named submitted-line fields instead of making
  consumers infer from `raw_elements`: `procedure_code_qualifier`,
  `procedure_code_set`, `procedure_code`, `procedure_modifiers`,
  `charge_amount`, `unit_measure_code`, `unit_count`, and
  `diagnosis_pointers`.
- Professional `SV1` and institutional `SV2` line positions are handled
  separately. `SV2` emits `revenue_code`.
- Stitching and balance projection read submitted charge/unit facts from the
  named fields, not from raw element positions.
- 837 provider role extraction now covers billing, rendering, referring,
  supervising, facility, attending, operating, and other provider references,
  with `reference_scope` distinguishing claim and service-line context.
- Binary journal key support includes the new 837 line fields.
- `README.md`, `model.md`, and `events.md` describe the current event surface.
- Tests have started moving out of `tests/test_parser.c`: shared helpers live in
  `tests/test_support.h`, low-level parser checks in `tests/test_x12_parser.c`,
  JSON/token checks in `tests/test_core.c`, store checks in `tests/test_store.c`,
  and focused 837 mapper coverage in `tests/test_837_mapper.c`.

Next logical implementation slices:

- Make `diagnosis_pointers` a structured array, matching
  `procedure_modifiers`.
- Parse the `CLM05` claim header composite into named fields for facility/place
  code, qualifier, and claim frequency.
- Attach `PRV` provider taxonomy/specialty facts to provider context.
- Continue 837I-specific claim facts: admission/discharge dates, patient status,
  occurrence codes, value codes, and DRG.

### 837 professional (837P)

Current support should be expanded from basic claim extraction into richer claim, provider, diagnosis, and service-line events.

Required event coverage:
- ClaimSubmitted.
- SubscriberReferenced.
- PatientReferenced.
- BillingProviderReferenced.
- RenderingProviderReferenced.
- ReferringProviderReferenced.
- SupervisingProviderReferenced.
- FacilityReferenced.
- DiagnosisReferenced.
- DiagnosisPointerReferenced.
- ServiceLineSubmitted.
- ProcedureReferenced.
- ChargeReferenced.

Implementation work:
- Validate common 837P loop structures.
- Expand provider role extraction.
- Extract diagnosis codes from HI segments.
- Extract diagnosis pointers from service lines.
- Extract service line procedure code, modifiers, units, charge amount, and service date.
- Preserve CLM01 and service line identifiers for later 835 matching.
- Add companion-guide-driven fixtures.

Minimum fixtures:
- Simple office visit.
- Multi-line professional claim.
- Claim with referring provider.
- Claim with rendering provider.
- Claim with multiple diagnoses and diagnosis pointers.

### 837 institutional (837I)

837I needs explicit support rather than treating it as a slightly different 837P. Institutional claims introduce facility, admission, discharge, bill type, revenue code, and inpatient/outpatient concepts.

Required event coverage:
- InstitutionalClaimSubmitted.
- FacilityReferenced.
- AttendingProviderReferenced.
- OperatingProviderReferenced.
- OtherProviderReferenced.
- SubscriberReferenced.
- PatientReferenced.
- DiagnosisReferenced.
- OccurrenceCodeReferenced.
- ValueCodeReferenced.
- RevenueLineSubmitted.
- ProcedureReferenced.
- ChargeReferenced.

Implementation work:
- Support institutional claim-specific loops.
- Extract bill type.
- Extract revenue codes.
- Extract admission date.
- Extract discharge date.
- Extract patient status.
- Extract occurrence codes.
- Extract value codes.
- Extract DRG when present.
- Extract facility identifiers.
- Extract attending and operating provider details.
- Extract institutional service/revenue lines.
- Preserve claim and revenue line identifiers for later 835 matching.

Minimum fixtures:
- Simple outpatient institutional claim.
- Inpatient stay with admission and discharge dates.
- Institutional claim with revenue codes.
- Institutional claim with attending provider.
- Institutional claim with value and occurrence codes.
- Institutional claim with DRG.

### 837 shared matching requirements

Both 837P and 837I must emit enough detail to make later 835 stitching reliable.

Shared requirements:
- Preserve tokenised claim identifiers.
- Preserve service/revenue line order.
- Preserve procedure, revenue code, charge, units, and dates where available.
- Emit source segment index and byte offset for every event.
- Keep PHI tokenised by default.
- Include enough provider and payer context to debug mismatches.

The stitcher should not have to infer everything from weak claim-level facts. The 837 event stream must carry the facts needed to pair submitted claim lines with remittance lines.

## Phase 2 - complete other core healthcare coverage

### 835 remittance
- Full CAS adjustment extraction.
- Service-level remittance modelling.
- Denial reason modelling.
- Payment balancing validation.
- Trace and payment reconciliation.

### 270/271 eligibility
- Coverage status modelling.
- Benefit extraction.
- Service-type support.
- Eligibility timelines.

### 834 enrollment
- Member lifecycle events.
- Coverage effective dates.
- Dependent modelling.
- Plan transitions.

## Phase 3 - claim lifecycle coverage

### 277CA claim acknowledgements
- Parse acknowledgements.
- Link acknowledgements to submitted claims.
- Generate ClaimAccepted events.
- Generate ClaimRejected events.
- Generate ClaimPending events.

### 276/277 claim status
- Parse claim status requests.
- Parse claim status responses.
- Generate ClaimStatusChanged events.
- Build claim lifecycle timelines.

### Future transactions
- 837D dental claims.
- 278 authorisation requests.
- 999 acknowledgements.
- TA1 acknowledgements.

## Phase 4 - validation engine

### Syntax validation
- Segment ordering.
- Required segment presence.
- Loop cardinality.
- Element count validation.
- Delimiter validation.

### Semantic validation
- Cross-segment consistency checks.
- Claim balancing.
- Subscriber/dependent relationships.
- Control number validation.
- 837P/837I-specific required data checks.

### Reporting
Emit structured validation events:
- WarningRaised
- ValidationFailed
- ValidationPassed

## Phase 5 - generic X12 infrastructure

### Schema registry
- Versioned transaction definitions.
- Loop definitions.
- Segment definitions.
- Element metadata.

### Transaction plugin model
- Independent transaction mappers.
- Version-specific mappings.
- Test fixture packs.

## Phase 6 - streaming operation

### Streaming parse API
- Parse from memory.
- Parse from stdin.
- Parse from sockets.
- Parse from message queues.

### Large file support
- Incremental parsing.
- Reduced memory usage.
- Streaming journal generation.

## Phase 7 - interoperability

### Output formats
- NDJSON.
- Journal.
- Parquet.
- Arrow.
- SQLite.

### Cloud patterns
- Lambda + S3.
- Azure Functions + Blob Storage.
- GCP Cloud Run.
- Kubernetes jobs.
- Airflow.
- Synapse/Fabric ingestion.

## Phase 8 - testing

### Fixture expansion
- CMS examples.
- Companion guide examples.
- Edge cases.
- Invalid files.
- 837P and 837I fixture packs.

### Differential testing
Compare output against:
- Stedi references.
- Commercial parsers.
- CMS examples.

## Reference material

Primary sources:
- CMS HIPAA 5010 documentation.
- CMS companion guides.
- CMS claims processing manual chapter 24.
- Stedi X12 reference browser.
- Stedi healthcare documentation.

## Non-goals

- Becoming a full EDI gateway.
- Replacing clearinghouses.
- Building a graphical integration platform.
- Hiding EDI behind proprietary abstractions.

The core philosophy remains:

EDI in -> auditable events out.
