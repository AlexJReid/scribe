# Event catalog

This catalog lists the current event names emitted or consumed by `scribe`.
Payloads are tokenised by default; PHI fields appear only when explicitly
running with `--include-phi` or when reading a PHI-resolved store.

| Event | Origin | Preview of conveyed information |
| --- | --- | --- |
| `AggregateVersionRecorded` | Derived notification outbox from `aggregate_stitcher.c` | Non-PHI fact that a claim aggregate version exists: `notification_id`, stitch `run_id`, ingest `source_run_id`, `aggregate_id`, `version`, `source_drop_id`, and last updating journal locator. |
| `ChargeTransactionObserved` | Local charge NDJSON via `journal_builder.c` | Provider-side charge context for a claim line: encounter, tokenised claim, claim type, line number, procedure, description, service date, and amount. |
| `ClaimAggregateUpdated` | Derived aggregate snapshot from `aggregate_stitcher.c` | Versioned claim aggregate state: keys, source/run metadata, claim/remit presence, patient identifiers, service lines, remittance joins, adjustments, and source event compaction. |
| `ClaimDateRecorded` | X12 837 `DTP` at claim scope via `x12_mapper_837.c` | Claim-level date qualifier, format, and value tied to a tokenised claim id. |
| `ClaimDiagnosesRecorded` | X12 837 `HI` via `x12_mapper_837.c` | Principal and other diagnosis codes plus raw diagnosis elements, tied to a tokenised claim id. |
| `ClaimLineDateRecorded` | X12 837 `DTP` at service-line scope via `x12_mapper_837.c` | Service-line date qualifier, format, and value tied to claim id and service line number. |
| `ClaimObserved` | X12 837 `CLM` via `x12_mapper_837.c` | Claim submission header: tokenised claim id and total billed charge amount. |
| `ClaimReferencedBillingProvider` | X12 837 `NM1*85` via `x12_mapper_837.c` | Billing provider reference for a claim: entity type, id qualifier, tokenised provider id, and optional PHI name. |
| `ClaimReferencedPatient` | X12 837 `NM1*QC` via `x12_mapper_837.c` | Patient reference for a claim: tokenised patient id, optional PHI name, and claim/service-line scope. |
| `ClaimReferencedRenderingProvider` | X12 837 `NM1*82` via `x12_mapper_837.c` | Rendering provider reference: tokenised provider id and scope, usually claim or service line. |
| `ClaimReferencedSubscriber` | X12 837 `NM1*IL` via `x12_mapper_837.c` | Subscriber/member reference for a claim: tokenised member id, optional PHI name, and claim scope. |
| `ClaimServiceLineRecorded` | X12 837 `SV1`/`SV2` via `x12_mapper_837.c` | Submitted service line: line number, procedure qualifier/code set/code, charge amount/unit details in raw elements. |
| `CoverageDateObserved` | X12 834 `DTP` via `x12_mapper_834.c` | Coverage-related date qualifier, format, and value for a member enrollment context. |
| `EligibilityBenefitObserved` | X12 271 `EB` via `x12_mapper_270_271.c` | Payer eligibility benefit answer: member/payer tokens, benefit info code, coverage level, service type, insurance type, plan description, amount/percent/quantity, auth and network indicators. |
| `EligibilityInquiryDateRecorded` | X12 270 `DTP` via `x12_mapper_270_271.c` | Eligibility inquiry date at transaction/member/benefit scope, with member and payer tokens when known. |
| `EligibilityInquiryDemographicsObserved` | X12 270 `DMG` via `x12_mapper_270_271.c` | Inquiry demographics used for eligibility matching: tokenised date of birth by default, optional PHI DOB/gender with `--include-phi`. |
| `EligibilityInquiryObserved` | X12 270 `BHT` via `x12_mapper_270_271.c` | Eligibility request header: eligibility id, purpose code, transaction date/time, type code, and raw BHT elements. |
| `EligibilityInquiryPartyReferenced` | X12 270 `NM1` via `x12_mapper_270_271.c` | Inquiry party reference: payer, provider, or member identity with tokenised ids and optional PHI names. |
| `EligibilityInquiryServiceTypeRequested` | X12 270 `EQ` via `x12_mapper_270_271.c` | Requested eligibility service type code for a member/payer context. |
| `EligibilityInquiryTraceRecorded` | X12 270 `TRN` via `x12_mapper_270_271.c` | Inquiry trace/correlation fields: trace type, trace number, originating company id, member/payer context. |
| `EligibilityResponseDateRecorded` | X12 271 `DTP` via `x12_mapper_270_271.c` | Response date at transaction/member/benefit scope, often eligibility effective/termination dates. |
| `EligibilityResponseDemographicsObserved` | X12 271 `DMG` via `x12_mapper_270_271.c` | Response-side demographics: tokenised DOB by default, optional PHI DOB/gender with `--include-phi`. |
| `EligibilityResponseObserved` | X12 271 `BHT` via `x12_mapper_270_271.c` | Eligibility response header: response id, purpose code, transaction date/time, type code, and raw BHT elements. |
| `EligibilityResponsePartyReferenced` | X12 271 `NM1` via `x12_mapper_270_271.c` | Response party reference: payer, provider, or member identity with tokenised ids and optional PHI names. |
| `EligibilityResponseTraceRecorded` | X12 271 `TRN` via `x12_mapper_270_271.c` | Response trace/correlation fields, usually linking the 271 back to the 270 inquiry trace. |
| `EncounterBalanceProjected` | Derived balance projection from `balance_projector.c` | Encounter-level ledger projection: claims, service lines, billed amounts, payer payments, contractual adjustments, patient responsibility, and current balance totals. |
| `EncounterObserved` | Local charge NDJSON via `journal_builder.c` | Encounter seed/context: encounter id, tokenised patient id, and synthetic flag when present. |
| `HealthCoverageObserved` | X12 834 `HD` via `x12_mapper_834.c` | Member health coverage detail: maintenance type, insurance line, plan coverage description, coverage level, and raw HD elements. |
| `MemberCoverageUpdated` | Derived aggregate snapshot from `coverage_stitcher.c` | Versioned member coverage state: member keys, optional PHI-resolved identifiers, enrollment dates, health coverage, eligibility service requests, benefit responses, source lineage, and lookup keys for member, payer, service type, and coverage dates. |
| `MemberEnrollmentChanged` | X12 834 `INS` via `x12_mapper_834.c` | Enrollment state/change facts: relationship code, maintenance type, benefit status, and raw INS elements. |
| `MemberReferenced` | X12 834 `NM1*IL` via `x12_mapper_834.c` | Member identity reference: entity type, id qualifier, tokenised member id, and optional PHI name. |
| `PatientPaymentObserved` | Local charge/payment NDJSON via `journal_builder.c` | Patient payment context consumed by balance projection: encounter/claim/line keys and amount. |
| `RefundObserved` | Local adjustment NDJSON via `journal_builder.c` | Refund context consumed by balance projection: encounter/claim/line keys and amount. |
| `RemittanceAdjustmentObserved` | X12 835 `CAS` via `x12_mapper_835.c` | Claim or service-line adjustment: scope, service line number, group code, reason codes, amounts, and quantities. |
| `RemittanceAdviceObserved` | X12 835 `BPR` + `TRN` via `x12_mapper_835.c` | Remittance/payment header: remittance id, trace fields, handling code, payment amount, method, debit/credit flag, and payment date. |
| `RemittanceClaimPaymentObserved` | X12 835 `CLP` via `x12_mapper_835.c` | Claim adjudication summary: tokenised claim id, status, billed/paid/patient responsibility, filing indicator, tokenised payer control number, facility and frequency codes. |
| `RemittanceClaimReferencedPatient` | X12 835 claim `NM1*QC` via `x12_mapper_835.c` | Patient reference inside remittance claim context: tokenised patient id and optional PHI name. |
| `RemittanceClaimReferencedSubscriber` | X12 835 claim `NM1*IL` via `x12_mapper_835.c` | Subscriber/member reference inside remittance claim context: tokenised member id and optional PHI name. |
| `RemittanceDateRecorded` | X12 835 `DTM` via `x12_mapper_835.c` | Remittance date at transaction/claim/service-line scope: qualifier and date value, with claim and service-line context when present. |
| `RemittancePartyReferenced` | X12 835 `N1` via `x12_mapper_835.c` | Remittance payer/payee party: entity code, optional PHI name, id qualifier, and tokenised payer/provider id. |
| `RemittanceServiceLinePaymentObserved` | X12 835 `SVC` via `x12_mapper_835.c` | Adjudicated service line: procedure qualifier/code, charge amount, paid amount, paid unit count, claim id, and line number. |
| `WriteoffObserved` | Local adjustment NDJSON via `journal_builder.c` | Provider writeoff context consumed by balance projection: encounter/claim/line keys and amount. |
