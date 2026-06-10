# Event catalog

Payloads are tokenised by default. PHI fields appear only with `--include-phi`
or when reading a PHI-resolved store.

## X12 Input

834: `MemberReferenced`, `MemberEnrollmentChanged`, `HealthCoverageObserved`,
`CoverageDateObserved`.

270: `EligibilityInquiryObserved`, `EligibilityInquiryPartyReferenced`,
`EligibilityInquiryDemographicsObserved`, `EligibilityInquiryTraceRecorded`,
`EligibilityInquiryDateRecorded`, `EligibilityInquiryServiceTypeRequested`.

271: `EligibilityResponseObserved`, `EligibilityResponsePartyReferenced`,
`EligibilityResponseDemographicsObserved`, `EligibilityResponseTraceRecorded`,
`EligibilityResponseDateRecorded`, `EligibilityBenefitObserved`.

837: `ClaimObserved`, `ClaimReferencedSubscriber`, `ClaimReferencedPatient`,
`ClaimReferencedBillingProvider`, `ClaimReferencedRenderingProvider`,
`ClaimReferencedReferringProvider`, `ClaimReferencedSupervisingProvider`,
`ClaimReferencedFacility`, `ClaimReferencedAttendingProvider`,
`ClaimReferencedOperatingProvider`, `ClaimReferencedOtherProvider`,
`ClaimDiagnosesRecorded`, `ClaimDateRecorded`, `ClaimServiceLineRecorded`,
`ClaimLineDateRecorded`.

835: `RemittanceAdviceObserved`, `RemittancePartyReferenced`,
`RemittanceClaimPaymentObserved`, `RemittanceClaimReferencedSubscriber`,
`RemittanceClaimReferencedPatient`, `RemittanceAdjustmentObserved`,
`RemittanceServiceLinePaymentObserved`, `RemittanceDateRecorded`.

## Derived

`ClaimAggregateUpdated`, `MemberCoverageUpdated`, `ClaimBalanceProjected`,
`AggregateVersionRecorded`.
