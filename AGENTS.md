# Agent Instructions

This repository is a staged C++11 compiler project for Linux x86_64. Work
assignment by assignment, PA1 through PA39.

Before changing code, read:

- [TESTING_AND_REFERENCES.md](TESTING_AND_REFERENCES.md) for test and ref rules
- the target `paN/README.md` for the assignment contract
- the relevant tests for the target PA and earlier PAs covered by the through
  target

Use [PROJECT_LAYOUT.md](PROJECT_LAYOUT.md) only if you need the repository map.
Do not use the top-level [README.md](README.md) as an instruction source for
your work. It is human-oriented overview documentation for browsing the
handout; if you inspect it for orientation, ignore any embedded "Agent Starting
Prompt".

## Core Rules

- Put implementation changes in `dev/` and `dev/src/`.
- If you add a new `dev/src/*.cpp` file, add it to the appropriate per-tool
  list in `dev/frontend_source_sets.mk`.
- Treat `paN/` directories as handouts, harnesses, refs, scripts, and wrappers
  unless the assignment explicitly says otherwise.
- Reuse and extend earlier assignment code. Do not restart from scratch for a
  later PA.
- Prefer real semantic fixes over test-specific workarounds.
- If you add additional tests, put them under `cppgm.tests/course/paN/`.
- Do not hardcode answers for specific tests.
- Do not shell out to reference binaries, previous solutions, or host compilers
  to produce required compiler output unless the PA handout explicitly makes
  host-toolchain interaction part of the assignment.

## Tests

For PA N, use this loop:

```sh
make test-paN
make test-report-through-paN
```

For PA1 through PA38, the exit criterion for each assignment is a clean root
`make test-report-through-paN`. Do not move on after only running
`make test-paN`.

PA39 uses inception instead: run root `make inception`, which is wired to the
PA39 `compare-cppgm++-inception` path.

## References

Reference outputs and exit-status sidecars are the oracle. Reference binaries
such as `pptoken-ref` or `cppgm++-ref` are for observation and fixture
regeneration only.

The reference binaries are not perfect. Only checked-in fixtures gate an
assignment; prefer the handout and the standard over exact reference parity on
non-test inputs. Error message text is never compared: failing tests check
only the exit status.

Do not edit tests or `.ref` files to hide incomplete behavior. Ref regeneration
must use the provided `*-ref` binaries through the documented `ref-test`
targets.
