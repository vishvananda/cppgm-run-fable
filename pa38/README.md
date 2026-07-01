## CPPGM Programming Assignment 38 (`lowir2native -O*`)

### Overview

PA38 adds machine-backend optimization levels to `lowir2native` and to the
shared native backend used by later `cppgm++` object and link-driver paths.

PA37 optimizes LowIR before backend lowering. PA38 starts after that boundary:
LowIR has already been translated into machine IR, and the backend must improve
the generated native path while preserving program behavior.

The questions for this assignment are:

- can `lowir2native -O1` perform local machine-IR cleanup?
- can `lowir2native -O2` perform whole-function machine-IR cleanup?
- can both levels preserve debug metadata and generated program behavior?
- can the optimized machine-IR path stay reusable by `cppgm++` rather than
  becoming a standalone `lowir2native` shortcut?

### Prerequisites

You should complete:

- PA28 for the baseline `lowir2native` backend
- PA37 for the explicit LowIR optimization stage

You will reuse the PA13 LowIR input language. Handwritten PA38 tests should use
the maintained LowIR surface, including explicit role metadata such as
`[role=entry]` where required.

### Starter Kit

The starter kit supplies:

- `pa38/Makefile`
- `pa38/lowir2native.cpp`, linked to the editable `dev/lowir2native.cpp`
- a `dev/lowir2native.cpp` scaffold based on `dev/lowir2native-scaffold.cpp`
- shared machine-IR and native backend support under `dev/src/`
- optional typed machine-IR model scaffolding in `dev/src/mir_model.h`, with
  shared register support in `dev/src/x86_register_model.h`
- test directories under `pa38/tests/`
- harness scripts under `pa38/scripts/`
- checked-in structural machine-IR and generated-program oracle sidecars

The expected implementation work is in `dev/lowir2native.cpp` and the shared
machine-IR/native backend modules under `dev/src/`, especially machine-IR
optimization and object-generation plumbing. The supplied LowIR parser,
machine-IR data model, object writer, linker helpers, and harness scripts are
support code; they do not complete the optimization assignment for you.

The harness uses checked-in sidecars as the oracle. There is no separate
`lowir2native-ref` binary in the starter kit.

### Command Line

PA38 requires these invocations:

```sh
lowir2native -O1 -o <program> <lowirfile>...
lowir2native -O2 -o <program> <lowirfile>...
lowir2native -O1 --dump-machine-ir <mirfile> <lowirfile>...
lowir2native -O2 --dump-machine-ir <mirfile> <lowirfile>...
lowir2native -O1 --dump-machine-ir <mirfile> -o <program> <lowirfile>...
lowir2native -O2 --dump-machine-ir <mirfile> -o <program> <lowirfile>...
```

`--help` and `-h` print usage information and exit successfully.

The `--target <target>` option is inherited from the native backend. Tests may
set the target through the harness environment, but the optimization
contract is independent of host-specific elapsed time.

`-O0` remains the PA28 baseline. PA38 must preserve that earlier behavior while
adding the explicit `-O1` and `-O2` backend optimization levels.

When `cppgm++` later emits native objects or executables at an optimization
level, it should use this same backend optimization pipeline after PA37 LowIR
optimization has produced the LowIR program to lower. Do not implement PA38 as
a display-only `lowir2native` transform that the compiler driver cannot reuse.

### Output Format

With `--dump-machine-ir <mirfile>`, `lowir2native` writes the optimized machine
IR dump to `<mirfile>`.

With `-o <program>`, `lowir2native` writes a native executable to `<program>`.
When both options are present, both outputs must be produced from the same
optimized machine-IR program.

`--dump-machine-ir` is the serialized view of the machine-IR program that the
native backend consumes. The object/native path may keep the MIR in memory, but
it should not use a different hidden representation with extra backend facts
that the MIR dump cannot express.

The same rule applies when the machine backend is reached through `cppgm++`:
optimization, object writing, and executable writing should consume the same
machine-IR facts that `--dump-machine-ir` can serialize.

The raw `x.ref.mir` file is kept as the debugging-oriented dump produced by
`--dump-machine-ir`. The primary backend-shape oracle is the structural
machine-IR sidecar `x.ref.cmir`, compared against a canonicalized form of the
generated `x.my.mir` dump. The generated native program's exit status and
standard output are behavior-preservation oracles layered on top of that
structural check.

### Error Handling

The tool must fail with a nonzero exit status when:

- neither `-o` nor `--dump-machine-ir` is provided
- an option requiring a path is missing that path
- there are no input files
- an input file cannot be read
- the input is not valid LowIR
- the target is unsupported
- a requested output file cannot be written
- native code generation or linking fails

For failure cases, exact diagnostic text is not part of the grading
contract. Output files after a failed run are undefined.

### Optimization Levels

To complete PA38, implement these backend optimization levels:

`-O1` is the local machine-improvement level. It must include these
semantic-preserving rewrites where safe:

- remove unconditional jumps to the immediately following block
- coalesce block-local integer and floating-point register copies
- remove redundant move chains and simple return shuffles
- clean up call-result and call-argument copies
- rematerialize cheap integer immediates into supported arithmetic,
  zero-compare, and call-argument instruction forms
- collapse conditional-branch plus unconditional-jump block tails when one
  target is the natural fallthrough block
- rewrite zero-comparison branches into direct `test reg, reg` machine IR when
  the backend supports that shape
- fold frame-address temporaries back into direct frame operands or direct
  `lea` call-argument setup where safe

`-O2` is the whole-function machine-improvement level. It must include all
`-O1` work and additionally:

- improve block layout by following unconditional jump traces so likely
  successors become natural fallthrough blocks
- remove callee-saved register preservation that is no longer needed after
  optimization
- recompute final stack reservation from the surviving frame state

Both levels must preserve valid debug metadata. Optimizations may choose to be
more conservative when a rewrite would make source locations misleading.

### Validation Modes

Machine-IR dumps contain presentation details, such as scratch-register choices,
frame offsets, and host target spelling, that are not always semantic
requirements. The PA38 harness canonicalizes permitted non-semantic differences
while still checking the required backend facts and generated program behavior.
Exact textual machine-IR matching is not a PA38 grading requirement unless a
test explicitly makes that shape part of the oracle.

### Testing

Run the PA38 suite with:

```sh
make test
```

`make test` runs:

- `tests/o1`
- `tests/o2`

Run the debug metadata preservation lanes with:

```sh
make test-debuginfo
```

`make test-debuginfo` runs:

- `tests/debuginfo/o1`
- `tests/debuginfo/o2`

These directories are organized by backend role and validation mode, not by
N3485 source-language clauses.

- `tests/o1` runs `lowir2native -O1` over LowIR inputs and checks local
  backend cleanup.
- `tests/o2` runs `lowir2native -O2` over LowIR inputs, repeats the `-O1`
  surface, and adds O2-only layout and frame cleanup cases.
- `tests/debuginfo/o1` and `tests/debuginfo/o2` run equivalent machine-IR
  rewrite cases carrying `!dbg(...)` metadata.

For each `.t` test, the harness builds with `--dump-machine-ir` and `-o`,
records implementation exit status, runs the generated program when the build
succeeds, and compares:

- implementation exit status
- optimized raw machine-IR dump, with the checked-in `x.ref.mir` retained for
  debugging and `x.ref.cmir` used as the structural machine-IR oracle
- generated-program exit status
- generated-program standard output, when relevant

Failed reference builds are judged by implementation exit status. Successful
reference builds are judged by structural machine-IR validation and
generated-program behavior.

### Out Of Scope

PA38 does not require:

- changing the LowIR optimizer from PA37
- redefining `lowir2native -O0`
- source-language semantic changes
- wall-clock performance grading
- global register allocation beyond the machine-IR cleanup contract
- instruction scheduling, vectorization, or target-specific peephole work not
  covered by the tests
- interprocedural backend optimization

### Handoff

PA39 uses the PA37 LowIR optimizer and the PA38 machine-backend optimizer as
part of the self-host ladder. By the end of PA38, optimized and unoptimized
native paths should remain deterministic enough for staged self-host builds and
test reruns.
