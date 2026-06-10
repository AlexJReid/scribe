# Agent notes

- C proof of concept for parsing synthetic healthcare EDI into journals, PHI
  vaults, read stores, aggregates, and balance projections.
- Core code is in `src/`; tests and synthetic fixtures are in `tests/`.
- Start with `README.md`; use `theory.md` for EDI, tokenisation, PHI, and run
  metadata notes.
- Keep changes small and C-style: explicit ownership, simple structs/functions,
  no broad refactors unless required.
- Treat PHI carefully. Existing PHI-looking fixture values are synthesized.
  Default paths should stay tokenised/non-PHI unless explicitly working on PHI
  flows.

## Commands

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run focused CLI checks with `build/scribe ...`.

## Editing

- Preserve warning-clean builds: `-Wall -Wextra -Wpedantic -Werror`.
- Add or adjust tests in `tests/test_parser.c` or fixtures when parser, mapper,
  journal, store, stitcher, aggregate, coverage, or projection behavior changes.
- `demo/` intentionally contains walked case study outputs. Do not add other
  generated local outputs such as `*.journal`, `*.sqlite`, or
  `*_aggregates.ndjson`.
- Prefer simple Redis-style C: direct data structures, obvious ownership, small
  functions, and no avoidable abstraction.
