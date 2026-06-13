# Caveats

## Parser Passes Are One-Shot

`x12_document_each_segment` currently tokenizes in place by writing NUL bytes
into the loaded document buffer. The production paths load, map, and free each
document once, so this is OK today. Treat an `x12_document_t` as consumed after
one segment pass unless the parser is changed to preserve the original buffer
or the API is renamed/documented as destructive.

## Stitcher Capacity Limits

Claim and coverage stitching use fixed-size in-memory arrays for aggregates,
source events, references, diagnoses, healthcare codes, provider taxonomies, and
adjustments. This keeps the C implementation simple, but it means larger or
unusual source drops can fail at internal limits before storage or journal
limits are reached.

Service lines per claim are the exception: they are heap-grown on demand (see
`claim_aggregate_add_service_line`) up to `STITCH_MAX_LINES_PER_CLAIM` (999, the
X12 LX maximum), because a service line is ~11 KB and embedding 999 inline in
every aggregate slot would make `stitch_state_t` over a gigabyte. The remaining
inline arrays (references-per-line, adjustments-per-line, etc.) still cap small;
an unusually wide claim can still hit those before 999 lines.

If the tool is pushed beyond demo/small-batch usage, the remaining fixed caps
should become explicit product limits, configurable limits, or dynamically
allocated collections with clearer error messages.

## Balance Projection

The claim aggregate now owns the balance arithmetic: each snapshot carries a
computed `balance` block per service line and per claim, with CO/PR bucketing and
the no-service-line envelope fallback applied once at snapshot time. The balance
projection reads those numbers and reshapes them into ledger output rather than
re-deriving them from raw charge/adjustment strings. Money parsing and formatting
live in the shared `src/util/money` helper used by both layers.

The projection's fixed claim and line caps (`BALANCE_MAX_CLAIMS`,
`BALANCE_MAX_LINES_PER_CLAIM`) no longer truncate silently — hitting a cap warns
on stderr and skips the remaining claims/lines while still emitting a valid
projection. If the tool moves beyond demo/small-batch usage these should still
become explicit, configurable, or dynamically sized limits.

## Money Parsing

The projection money parser (`parse_money`) now rejects strings with trailing
non-numeric characters or no digits, rounds a third fractional digit half-up,
and reports overflow as `X12_ERR_INVALID_ARGUMENT` instead of silently
overflowing. Empty/absent fields are still treated as zero. Any future
money-handling path outside the projection should hold to the same contract.
