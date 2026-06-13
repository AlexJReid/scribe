# Caveats

## Parser Passes Are One-Shot

`x12_document_each_segment` currently tokenizes in place by writing NUL bytes
into the loaded document buffer. The production paths load, map, and free each
document once, so this is OK today. Treat an `x12_document_t` as consumed after
one segment pass unless the parser is changed to preserve the original buffer
or the API is renamed/documented as destructive.

## Stitcher Capacity Limits

Claim and coverage stitching use fixed-size in-memory arrays for aggregates,
source events, service lines, references, diagnoses, healthcare codes, provider
taxonomies, and adjustments. This keeps the C implementation simple, but it
means larger or unusual source drops can fail at internal limits before storage
or journal limits are reached.

If the tool is pushed beyond demo/small-batch usage, these caps should become
explicit product limits, configurable limits, or dynamically allocated
collections with clearer error messages.

## Balance Projection Is Provisional

The current balance projection duplicates a view that may be better derived
from aggregate state directly. It also has its own fixed claim and line caps.
Consider removing it from the core path, or rethinking it later as a thin
consumer of aggregate snapshots instead of a separate modeling layer.

## Money Parsing Is Lenient

The projection money parser accepts partial numeric strings and does not check
integer overflow. That is not a current aggregate-stitching issue, but any
future money-handling path should validate the full string and reject overflow.
