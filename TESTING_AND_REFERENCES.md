# Testing And References

Run build and test targets from the repository root unless a PA handout says to
use a narrower command.

## Common Root Targets

```sh
make build
make test
make test-paN
make test-report-through-paN
make test-report
make test-strict
make inception
```

- `make build` builds the compiler tools in `dev/`.
- `make test` builds once, then runs the assignment tests.
- `make test-paN` runs one assignment.
- `make test-report-through-paN` runs the report suite through PA N.
- `make test-report` runs the broad keep-going report.
- `make test-strict` runs stricter suites used by selected later assignments.
- `make inception` runs the final PA39 self-host comparison.

The exit criterion for PA N is a clean root `make test-report-through-paN`.

## Assignment-Local Targets

Inside an assignment directory:

```sh
make
make test
make check TEST=tests/path/to/case.t
make check TEST='tests/path/to/100-*.t'
make check TEST='tests/path/to/a.t tests/path/to/b.t'
```

`TEST=` accepts one checked-in test file, a quoted shell glob, or a quoted
space-separated list of files and globs. Later assignments with multiple test
kinds can route mixed entries to the right local runner, for example a
preprocessor case plus a compile case. Use local targets for quick iteration,
then return to the root through target before considering the assignment
complete.

## Compiler Selection

You may pass compilers explicitly:

```sh
make CXX=g++ CPPGM_HOST_CXX=g++
```

For normal host builds, `CXX` and `CPPGM_HOST_CXX` should usually match. For
PA39 self-host work, `CXX` may be `../dev/cppgm++` while `CPPGM_HOST_CXX`
remains a real host compiler.

## Test Locations

- Assignment-local tests live in `paN/tests/`.
- Shared course tests live in `cppgm.tests/course/paN/`.
- Later PA handouts may describe additional folders such as strict, debuginfo,
  object-inspection, or link tests.

Add focused regression tests for new bugs. Put shared student tests under
`cppgm.tests/course/paN/` when they should travel with the course-wide harness;
put PA-local tests under `paN/tests/` when they are specific to one assignment.

## References

Reference outputs, stdout refs, exit-status refs, and inspect refs are the test
oracle. Do not edit them to hide an incomplete implementation.

Reference binaries such as `pptoken-ref` or `cppgm++-ref` are provided for
observing expected behavior and regenerating reference fixtures. They must not
be used by your compiler implementation.

The reference binaries are not perfect. Only checked-in fixtures gate an
assignment. Synthesizing new inputs is fine, but reference behavior outside
the test suites is intended to be correct, not guaranteed; prefer the handout
and the standard over exact reference parity. Error message text is never
compared: failing tests check only the exit status.

The repository does not store the large binary payloads in Git. The checked-in
`*-ref` wrappers automatically download, verify, and unpack the pinned
reference-binary bundle the first time a reference tool is needed. To fetch the
bundle before running a ref target, use:

```sh
make reference-binaries
```

The ref regeneration targets use the provided reference binaries:

```sh
make ref-test-paN
make -C paN ref-test
```

These targets intentionally fail if the needed reference binary cannot be
downloaded or verified. There is no fallback to the implementation under test.

## Strict And Debug Fixtures

Some later assignments include optional stricter comparisons, witness fixtures,
debug-info fixtures, or object/link inspection checks. Run the targets named in
the PA handout when you touch that surface. The broad root commands are:

```sh
make test-strict
make ref-test-strict
make ref-test-debuginfo
```

Strict/reference regeneration is for maintaining fixtures from the provided
reference tools, not for making current incorrect output pass.
