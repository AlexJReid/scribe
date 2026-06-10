# Full EDI Support Roadmap

## Goal

Evolve scribe from a healthcare-focused X12 event extraction tool into a robust EDI processing platform while preserving its Unix philosophy:

- Read files or streams.
- Emit auditable NDJSON events.
- Compose with jq, DuckDB, Spark, Lambda, cron, S3 triggers, and message queues.
- Avoid becoming a heavyweight integration server.

Reference sources include CMS documentation, companion guides, and Stedi healthcare documentation. These are used to prioritise transaction coverage and validate behaviour, not as implementation dependencies.

## Phase 1 - Complete Core Healthcare Coverage

### 837 Professional (837P)
- Validate common loop structures.
- Expand provider entity extraction.
- Extract diagnosis pointers.
- Extract service line details.
- Improve claim status and adjustment modelling.
- Add companion-guide-driven fixtures.

### 837 Institutional (837I)
- Support institutional claim-specific loops.
- Revenue codes.
- Bill types.
- Facility identifiers.
- Institutional service lines.

### 835 Remittance
- Full CAS adjustment extraction.
- Service-level remittance modelling.
- Denial reason modelling.
- Payment balancing validation.
- Trace and payment reconciliation.

### 270/271 Eligibility
- Coverage status modelling.
- Benefit extraction.
- Service-type support.
- Eligibility timelines.

### 834 Enrollment
- Member lifecycle events.
- Coverage effective dates.
- Dependent modelling.
- Plan transitions.

## Phase 2 - Claim Lifecycle Coverage

### 277CA Claim Acknowledgements
- Parse acknowledgements.
- Link acknowledgements to submitted claims.
- Generate ClaimAccepted events.
- Generate ClaimRejected events.
- Generate ClaimPending events.

### 276/277 Claim Status
- Parse claim status requests.
- Parse claim status responses.
- Generate ClaimStatusChanged events.
- Build claim lifecycle timelines.

### Future Transactions
- 837D dental claims.
- 278 authorisation requests.
- 999 acknowledgements.
- TA1 acknowledgements.

## Phase 3 - Validation Engine

### Syntax Validation
- Segment ordering.
- Required segment presence.
- Loop cardinality.
- Element count validation.
- Delimiter validation.

### Semantic Validation
- Cross-segment consistency checks.
- Claim balancing.
- Subscriber/dependent relationships.
- Control number validation.

### Reporting
Emit structured validation events:
- WarningRaised
- ValidationFailed
- ValidationPassed

## Phase 4 - Generic X12 Infrastructure

### Schema Registry
- Versioned transaction definitions.
- Loop definitions.
- Segment definitions.
- Element metadata.

### Transaction Plugin Model
- Independent transaction mappers.
- Version-specific mappings.
- Test fixture packs.

## Phase 5 - Streaming Operation

### Streaming Parse API
- Parse from memory.
- Parse from stdin.
- Parse from sockets.
- Parse from message queues.

### Large File Support
- Incremental parsing.
- Reduced memory usage.
- Streaming journal generation.

## Phase 6 - Interoperability

### Output Formats
- NDJSON.
- Journal.
- Parquet.
- Arrow.
- SQLite.

### Cloud Patterns
- Lambda + S3.
- Azure Functions + Blob Storage.
- GCP Cloud Run.
- Kubernetes jobs.
- Airflow.
- Synapse/Fabric ingestion.

## Phase 7 - Testing

### Fixture Expansion
- CMS examples.
- Companion guide examples.
- Edge cases.
- Invalid files.

### Differential Testing
Compare output against:
- Stedi references.
- Commercial parsers.
- CMS examples.

## Reference Material

Primary sources:
- CMS HIPAA 5010 documentation.
- CMS companion guides.
- CMS claims processing manual chapter 24.
- Stedi X12 reference browser.
- Stedi healthcare documentation.

## Non-Goals

- Becoming a full EDI gateway.
- Replacing clearinghouses.
- Building a graphical integration platform.
- Hiding EDI behind proprietary abstractions.

The core philosophy remains:

EDI in -> auditable events out.
