## CPPGM Programming Assignment 39 (Inception)

### Overview

PA39 is the inception assignment. To complete PA39, make `cppgm++` build
`cppgm++` and have that rebuilt compiler match the host-seeded build. In
Makefile terms, the main target is:

```sh
make compare-cppgm++-inception CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

This builds `cppgm++-self`, builds `cppgm++-inception` with that self-built
compiler, and compares the two outputs byte for byte. Passing that comparison
means the compiler can reproduce itself from its own output.

The `test-through-*` preservation ladder is not the final product. It is the
debugging path that helps you reach inception one stage at a time. The earlier
programming assignments build a compiler sequentially by adding one language,
semantic, lowering, runtime, or backend surface at a time; the PA39 ladder uses
the same idea for self-hosting. Each rung runs the already-completed assignment
tests with a self-built checkpoint binary, so missing functionality usually
shows up at the first stage that needs it instead of only as a full
`cppgm++` inception failure.

### What PA39 Tests

PA39 does not add a new language feature, command-line mode, object format, or
runtime ABI. It reuses the compiler implementation and checks that the existing
implementation can compile itself reproducibly.

It also does not relax the LowIR or MIR boundaries established earlier. If a
self-host or inception failure shows that object output behaves differently
from output reconstructed through LowIR/MIR text, treat that as an earlier
compiler contract bug rather than adding a PA39-only side channel.

If the earlier assignment tests cover every language, lowering, runtime,
linking, and optimization feature used by your implementation, PA39 should be a
straightforward build plus a few reproducibility cleanups. In practice, the
self-build usually finds missing coverage. A failure in PA39 is usually evidence
that an earlier compiler surface accepts the assignment tests but still has a
bug in code that the compiler implementation itself happens to use.

### Prerequisites

Complete PA38 before starting PA39. PA39 reuses:

- the PA1-PA9 frontend tools
- the cumulative `cppgm++` compiler
- the native, object, runtime, and hosted compile/link surfaces
- the PA37/PA38 optimization surfaces
- the earlier `pa1` through `pa38` tests for preservation checks

PA39 also needs a host C++ compiler and linker. The host compiler links staged
object files and provides the hosted configuration used by the compiler build.

### Build Variables

Use two compiler variables when running PA39:

- `CXX=../dev/cppgm++` selects the course compiler as the compiler under test.
- `CPPGM_HOST_CXX=<host-cxx>` selects the host C++ compiler used for linking
  checkpoint programs and generating hosted compiler configuration.

For example:

```sh
make compare-cppgm++-inception CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

If `../dev/cppgm++` does not exist yet, the PA39 Makefile first builds it with
`CPPGM_HOST_CXX`.

PA39 applies a per-file wall timeout to self-hosted compile steps so a single
source file cannot stall the ladder indefinitely. The defaults are intentionally
generous:

- `INCEPTION_SELFHOST_COMPILE_TIMEOUT_SEC=900` for `*-self` object compiles
- `INCEPTION_INCEPTION_COMPILE_TIMEOUT_SEC=3600` for `*-inception` object
  compiles

A timeout usually points to a performance or nontermination bug in an earlier
compiler surface. Prefer reducing and fixing that bug over increasing the
timeout.

The compile wrapper also applies a default per-command RSS cap. By default,
commands that exceed 8 GiB RSS fail with `EXIT_OOM`; set
`CPPGM_RUN_MAX_RSS_KB=0` to disable that guard for diagnosis, or set it to a
larger value if you have already reduced the memory use and need a temporary
validation run.

The initial `*-self` build uses the normal make parallelism default. The
`*-inception` subbuild is capped separately with `INCEPTION_BUILD_JOBS`, which
defaults to the smaller of the available CPU count and
`INCEPTION_DEFAULT_BUILD_JOB_CAP=8`. Raise that only if the machine has enough
memory for several simultaneous self-built compiler processes.

### Inception Targets

The focused PA39 goal is:

```sh
make cppgm++-inception CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make compare-cppgm++-inception CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

Useful broader targets are:

```sh
make inception CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make compare-inception CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make bitcmp CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

`inception` builds all inception checkpoint binaries. `compare-inception` and
`bitcmp` compare all self-built checkpoint binaries against their inception
versions.

During a normal in-tree inception build, PA39 also checks each newly built
inception object before the final target link finishes. The check does not
wait for the final executable link: it byte-compares the new inception `.o`
against the corresponding self-host `.o` and stops at the first mismatch. That
catches object-level reproducibility drift as soon as the responsible source
file finishes compiling while still leaving the final linked executable compare
as the PA39 result. The restored-self diagnostic target disables this object
check because it may not have a matching self-host object tree.

For both self-host and inception checkpoint binary builds, PA39 schedules
missing `.o` inputs before stale existing inputs by default. If an earlier run
stopped partway through the object tree, this usually reaches the next never-seen
compile failure faster. The final link still uses the canonical object order, so
the scheduling change does not affect reproducibility. Set
`INCEPTION_BUILD_MISSING_FIRST=0` to disable this behavior.

### Intermediate Ladder

To build one checkpoint:

```sh
make pptoken-self CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make cppgm++-self CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

To build checkpoints through a point:

```sh
make through-cy86 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make through-cppgm++ CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make through-lowir2native CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

To run preservation tests through an assignment stage:

```sh
make test-through-pa9 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make test-through-pa38 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

To test one stage:

```sh
make test-pa1 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make test-pa10 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
make test-pa37 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++
```

The test targets reuse the earlier assignment harnesses. PA39 changes which
binary those harnesses run; it does not change the expected PA1 through PA38
outputs.

### Single-Object Probes

During PA39, changing one `dev/src/*.cpp` file and then rerunning a canonical
self-host target can rebuild every self-host object. That rebuild is correct:
the object files depend on the compiler binary that produced them, and mixing
objects from different compiler generations would make the inception result
untrustworthy.

For quick iteration, use a scratch object probe instead:

```sh
make probe-self-object SOURCE=../dev/src/semantic_output.cpp
make probe-self-object SOURCE=dev/src/semantic_output.cpp
make probe-self-object SOURCE=../dev/cppgm++.cpp
```

The probe uses the PA39 self-host compile flags and timeout, but writes to
`../obj/pa39/probe/selfhost/...` instead of the canonical
`../obj/pa39/selfhost/...` tree. It prints the compiler, source, object, and
depfile paths. By default it uses `PROBE_CXX=../dev/cppgm++`; set `PROBE_CXX`
if you need to test a different compiler binary.

If the canonical checkpoint objects already exist, you can link one scratch
replacement object into a scratch binary:

```sh
make probe-self-link SOURCE=../dev/src/semantic_output.cpp PROBE_TARGET=cppgm++
```

`probe-self-link` refuses to link if the probed object is not part of
`PROBE_TARGET`, or if any required canonical object is missing. This keeps the
probe from silently becoming a mixed or incomplete build. Probe targets are
only for diagnosis; final validation still requires the canonical PA39 targets.

### Checkpoint Ownership

The checkpoint used for each assignment stage is:

- `pptoken-self`: PA1
- `posttoken-self`: PA2
- `ctrlexpr-self`: PA3
- `macro-self`: PA4
- `preproc-self`: PA5
- `recog-self`: PA6
- `nsdecl-self`: PA7
- `nsinit-self`: PA8
- `cy86-self`: PA9
- `cppgm++-self`: PA10-PA12, PA14-PA27, and PA29-PA36
- `lowir2cy86-self`: PA13
- `lowir2native-self`: PA28 and PA38
- `abimangle-self`: PA30
- `lowiropt-self`: PA37

Checkpoint source sets are fixed by `../dev/frontend_source_sets.mk`. Do not
replace that with a generated source scan; PA39 is checking whether the known
implementation source sets can be rebuilt reproducibly.

### Working Through Failures

Treat PA39 failures as compiler bugs until proven otherwise.

Do not rewrite tests around a failure, and do not add a self-hosting special
case just to make the build move forward. Find the first incorrect behavior and
fix the underlying parser, semantic, lowering, optimizer, backend, runtime, or
reproducibility bug.

When an inception compile is far slower than the corresponding host-seeded
compile, look for divergent algorithmic behavior in the self-built compiler,
not just less optimized machine instructions. A large slowdown often means the
self-built compiler is following a different branch, repeatedly redoing
semantic work, or missing a cache because an earlier stage miscompiled the
compiler itself.

A useful workflow is:

1. Find the first failing checkpoint, source file, or preservation test.
2. Reduce the failure to the smallest source that still fails.
3. Identify the earliest assignment surface that owns that behavior.
4. Add the reducer as a focused test under the matching
   `cppgm.tests/course/paN` directory while you work on the fix.
5. Fix the underlying compiler bug and rerun the narrow stage before returning
   to the broader `test-through-*` or inception target.

For example, if `cppgm++-self` fails because a construct in `dev/src/*.cpp` is
miscompiled, reduce that construct and place the focused test in the earliest
`cppgm.tests/course/paN` directory that should have covered it. If
`compare-cppgm++-inception` builds both compilers but the bytes differ, look for
reproducibility issues such as unstable output order, generated configuration
drift, embedded paths, timestamps, or linker determinism.

The best PA39 fixes usually improve an earlier assignment surface and leave a
small focused test behind.
