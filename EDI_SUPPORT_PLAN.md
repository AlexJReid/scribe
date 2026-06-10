# Full EDI Support Roadmap

## Goal

Evolve scribe from a healthcare-focused X12 event extraction tool into a robust EDI processing platform while preserving its Unix philosophy:

- Read files or streams.
- Emit auditable NDJSON events.
- Compose with jq, DuckDB, Spark, Lambda, cron, S3 triggers, and message queues.
- Avoid becoming a heavyweight integration server.

## Phase 1 - Complete Current Healthcare Coverage

### 837 Professional (837P)

- Validate common loop structures.
- Expand provider entity extraction.
- Extract diagnosis pointers.
- Extract service line details.
- Improve claim status and adjustment modelling.
- Add companion-guide-driven fixtures.

Reference:
- CMS 837P companion guide.

### 837 Institutional (837I)

- Support institutional claim-specific loops.
- Revenue codes.
- Bill types.
- Facility identifiers.
- Institutional service lines.

Reference:
- CMS 837I companion guide.

### 835 Remittance

- Full CAS adjustment extraction.
- Service-level remittance modelling.
- Denial reason modelling.
- Payment balancing validation.
- Trace and payment reconciliation.

Reference:
- CMS 835 companion guide.

### 270/271 Eligibility

- Coverage status modelling.
- Benefit extraction.
- Service-type support.
- Eligibility timelines.

Reference:
- CMS HETS companion guide.

### 834 Enrollment

- Member lifecycle events.
- Coverage effective dates.
- Dependent modelling.
- Plan transitions.

## Phase 2 - Validation Engine

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

## Phase 3 - Generic X12 Infrastructure

### Schema Registry

- Versioned transaction definitions.
- Loop definitions.
- Segment definitions.
- Element metadata.

### Transaction Plugin Model

Support loading additional transaction mappers:

- 276/277 claim status.
- 278 authorization.
- 820 payments.
- 999 acknowledgements.
- TA1 acknowledgements.

## Phase 4 - Streaming Operation

### Streaming Parse API

Introduce callback-based APIs:

- Parse from memory.
- Parse from stdin.
- Parse from sockets.
- Parse from message queues.

### Large File Support

- Incremental parsing.
- Reduced memory usage.
- Streaming journal generation.

## Phase 5 - Interoperability

### Output Formats

- NDJSON.
- Journal.
- Parquet.
- Arrow.
- SQLite.

### Cloud Patterns

Document:

- Lambda + S3.
- Azure Functions + Blob Storage.
- GCP Cloud Run.
- Kubernetes jobs.
- Airflow.
- Synapse/Fabric ingestion.

## Phase 6 - Testing

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

## Non-Goals

- Becoming a full EDI gateway.
- Replacing clearinghouses.
- Building a graphical integration platform.
- Hiding EDI behind proprietary abstractions.

The core philosophy remains:

EDI in -> auditable events out.
