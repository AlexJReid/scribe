# Synthetic stroke encounter fixture

This directory models a synthetic encounter with multiple claims. It is not real
patient data.

Encounter:

- `encounter_id`: `ENC-SYN-STROKE-001`
- Clinical story: stroke evaluation with CT without contrast, CT with contrast,
  MRI brain imaging, outpatient rehab, and neurology follow-up during recovery

Files:

- Facility imaging claim
  - `facility_837.edi`: submission for the three imaging services
  - `facility_835.edi`: payer remittance
- Professional imaging claim
  - `professional_837.edi`: radiologist interpretation submission
  - `professional_835.edi`: payer remittance
- Rehab claim
  - `rehab_837.edi`: outpatient rehab submission
  - `rehab_835.edi`: payer remittance
- Neurology claim
  - `neurology_837.edi`: follow-up submission during recovery
  - `neurology_835.edi`: payer remittance
- `charge_transactions.ndjson`: example upstream charge/encounter rows that a
  future charge transaction importer could map into ledger entries. This file
  seeds `ENC-SYN-STROKE-001` and links each synthetic claim back to that
  encounter before the 837 and 835 files are stitched in
- `expected_balance_projection.json`: compact reference projection shape. The
  generated `balance` projection also includes per-line ledger entries

Current parser behaviour:

- 837 `CLM01` and 835 `CLP01` should tokenise into the same `claim_id` value for
  each claim
- 835 `CLP07` should tokenise under the `payer_claim_control_number` namespace
- Service lines can be matched by claim ID, service line number, procedure code,
  charge amount, date, and line order

Expected encounter-level balance from the 835 files:

- Total billed: `3720.00`
- Payer paid: `2340.00`
- Contractual adjustments: `830.00`
- Patient responsibility: `550.00`
