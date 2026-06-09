# Synthetic stroke encounter fixture

Synthetic encounter `ENC-SYN-STROKE-001` models stroke evaluation and recovery:
CT imaging, MRI, outpatient rehab, and neurology follow-up. It is not real
patient data.

## Files

- `charge_transactions.ndjson`: encounter, charge, payment, writeoff, and refund
  rows that seed the local side of the ledger.
- `coverage_834.edi`: synthetic member enrollment context.
- `eligibility_270.edi`: eligibility inquiry for the service window.
- `eligibility_271.edi`: payer eligibility response with benefit context.
- `facility_837.edi` / `facility_835.edi`: facility imaging claim and remit.
- `professional_837.edi` / `professional_835.edi`: radiologist claim and remit.
- `rehab_837.edi` / `rehab_835.edi`: outpatient rehab claim and remit.
- `neurology_837.edi` / `neurology_835.edi`: follow-up claim and remit.
- `expected_balance_projection.json`: compact reference balance shape.

## Expectations

- 834, 270, and 271 records journal tokenised coverage/member evidence.
- 837 `CLM01` and 835 `CLP01` tokenise into the same `claim_id` for each claim.
- 835 `CLP07` tokenises under `payer_claim_control_number`.
- Service lines can match by claim ID, line number, procedure code, charge
  amount, service date, and line order.

Expected encounter balance:

```text
billed:                 3720.00
payer paid:             2340.00
contractual adjustment:  830.00
patient responsibility:  550.00
```
