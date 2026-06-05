# Agent Notes

- C17/CMake proof of concept for parsing 834/835/837 data into journals, PHI vaults, read stores, aggregates, and balance projections.
- Core code is in `src/`; tests and synthetic fixtures are in `tests/`.
- Start with `README.md` for architecture decisions and run commands; use `theory.md` for 837/835, tokenisation, and PHI background.
- Keep changes small and C-style: explicit ownership, simple structs/functions, no broad refactors unless required.
- Treat PHI carefully. Existing PHI-looking fixture values are synthesized; the case study is only inspired by a UK, non-US healthcare episode I had. Default paths should stay tokenised/non-PHI unless explicitly working on PHI flows.

## Commands

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run focused CLI checks with `build/scribe ...`; see `README.md` for stroke demo commands.

## Editing

- Preserve warning-clean builds: `-Wall -Wextra -Wpedantic -Werror`.
- Add/adjust tests in `tests/test_parser.c` or fixtures when parser, mapper, journal, store, or projection behavior changes.
- `demo/` intentionally contains walked case study outputs. Do not add other generated local outputs such as `*.journal`, `*.sqlite`, or `*_aggregates.ndjson`.
