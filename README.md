# cppgm

## Fable Ralph Run

This repository records the `fable` Ralph compiler run. View the
[exported run](https://storage.googleapis.com/ralph-run-viewer-zippy-960/index.html?run=fable-claude-fable-5-xhigh%2Frun&view=run),
the maintained [cppgm-extended](https://github.com/vishvananda/cppgm-extended)
project, and the exported
[cppgm-assignments](https://github.com/vishvananda/cppgm-assignments).

`cppgm` is a staged course project for building a C++11 compiler. The
assignments start with preprocessing tokens and grow into a practical
self-hosting compiler toolchain.

This repository contains the starter sources, assignment handouts, tests,
reference outputs, and reference-binary wrappers for PA1 through PA39. The
assignment sequence is cumulative: later assignments build on earlier work, and
most shared implementation work lives in `dev/` and `dev/src/`.

Start each assignment by reading its handout: `paN/README.md`.

## License

Copyright 2026 The cppgm-extended Authors.

The repository-maintained sources and support files are licensed under the
Apache License, Version 2.0. See [LICENSE](LICENSE) and [AUTHORS](AUTHORS).
See [NOTICE](NOTICE) for third-party and archival attribution.

## Archival Note

PA1 through PA9 include assignment material from the CPP Grandmasters challenge.
The original site appears to be defunct, so these materials are preserved here
for archival and continuity purposes. See [NOTICE](NOTICE) for the PA1-PA9
copyright attribution and removal contact note.

## Environment

The course target environment is Linux x86_64.

You should have:

- GNU make
- a C++ compiler with C++11 support, normally `g++`
- Bash
- Perl
- `curl` or `wget` for automatic reference-binary downloads
- common Unix tools such as `diff`, `grep`, `sed`, `awk`, and `xxd`
- common archive/checksum tools such as `tar` and `sha256sum`
- binutils tools such as `ar`, `nm`, `objdump`, and `readelf` for later
  native/toolchain assignments

If you use an autonomous coding agent, run it in an isolated sandbox. Later
assignments build and execute generated programs.

## Where Things Are

Implementation changes go in `dev/` and `dev/src/`. Assignment directories
mostly hold handouts, tests, references, scripts, and thin wrappers around the
shared implementation.

See [PROJECT_LAYOUT.md](PROJECT_LAYOUT.md) for the full repository layout and
PA1-PA39 assignment map.

## Build And Test

Useful root commands:

```sh
make build
make test-paN
make test-report-through-paN
make test-report
make test-strict
make inception
```

The exit criterion for PA N is a clean root `make test-report-through-paN`.
That target runs the report suite through the milestone and catches regressions
in earlier assignments before you move on.

No separate setup is normally required. Reference binaries download
automatically the first time a `*-ref` tool or ref regeneration target needs
them. To fetch them eagerly, run:

```sh
make reference-binaries
```

See [TESTING_AND_REFERENCES.md](TESTING_AND_REFERENCES.md) for the complete
testing workflow, reference-binary policy, and ref regeneration commands.

You may pass a compiler explicitly:

```sh
make CXX=g++ CPPGM_HOST_CXX=g++
```

For normal host builds, `CXX` and `CPPGM_HOST_CXX` should usually be the same
compiler. For PA39 self-host experiments, `CXX` may be `../dev/cppgm++`, while
`CPPGM_HOST_CXX` should remain a real host compiler.

## Working On An Assignment

1. Read `paN/README.md`.
2. Run the assignment tests once before making changes:

   ```sh
   make test-paN
   ```

3. Edit the relevant files in `dev/` and `dev/src/`.
4. Add focused tests as you work. Shared student tests belong under
   `cppgm.tests/course/paN/`; assignment-local tests live under `paN/tests/`.
5. Run the assignment test and the through-milestone report:

   ```sh
   make test-paN
   make test-report-through-paN
   ```

Keep the implementation cumulative. Reuse earlier code and refactor shared
pieces when later assignments make the original shape too narrow.

## C++ Reference

The project targets C++11. Use `doc/n3485.txt` as the standard reference when a
PA handout cites standard behavior.

## Agent Starting Prompt

If you use a coding agent, a good starting prompt is:

```text
Read AGENTS.md and follow it exactly. Continue the cppgm assignment sequence in
order. Reuse and extend existing code instead of starting over. Follow the
checked-in tests and each paN/README.md. Do not change tests or refs to hide
incomplete implementation. Stop only when the relevant root test target passes.
```
