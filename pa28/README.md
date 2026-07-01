## CPPGM Programming Assignment 28 (lowir2native)

### Overview

Write a C++ application called `lowir2native` that takes as input a set of LowIR source
files and writes a native executable program.

PA28 replaces PA13 `lowir2cy86` as the primary backend path. The input language is still
LowIR, but the required output is no longer CY86 text. Instead, PA28 lowers LowIR directly
to native code and native program data.

The intent of this milestone is:

- keep LowIR as the long-term compiler backend boundary
- reuse the PA9 native backend knowledge without making CY86 the compiler IR
- leave room for later optimization and additional native backends

### Prerequisites

You should complete Programming Assignment 27 before starting this assignment.

You will want to reuse:

- the PA13 LowIR parser and LowIR specification
- the PA9 native backend pieces: instruction encoding, executable container writing, and
  startup/runtime glue
- any shared lowering or assembler abstractions that help you separate:
  - LowIR -> native instruction selection
  - native code/data emission
  - final executable image construction

PA28 tests execute generated native programs. Your development host therefore
needs an x86-64 Linux execution environment. With no `--target`, the tool should
emit a Linux executable. The target name used by the course is `linux`.

### Starter Kit

The starter kit contains:

- `pa28/README.md`, `pa28/Makefile`, and the test scripts in `pa28/scripts/`
- a student-editable `dev/lowir2native.cpp` starter scaffold
- the `pa28/lowir2native.cpp` symlink back to `../dev/lowir2native.cpp`
- shared support sources and headers under `dev/src/`
- optional typed LowIR and machine-IR model scaffolding in
  `dev/src/lowir_model.h` and `dev/src/mir_model.h`, with shared
  exported-symbol and register support in `dev/src/ir_symbol_model.h` and
  `dev/src/x86_register_model.h`
- a local test suite under `pa28/tests/`
- the grammar for this assignment called `pa28.gram`
- the authoritative LowIR specification in `../pa13/lowir.md`
- an HTML grammar explorer of `pa28.gram` in the sub-directory `grammar/`
- checked-in golden result files under `tests/`
- `tests/strict/` for raw-MIR oracle tests
- `tests/structural/` for canonical-MIR oracle tests
- `tests/behavior/` for generated-program behavior tests without a machine-IR oracle

Students should implement the assignment in `dev/lowir2native.cpp` and any reusable
student-owned helpers they add under `dev/src/`. The assignment directory, grammar files,
test fixtures, comparison scripts, and checked-in reference outputs are support
files, not implementation files to edit for normal solutions. The shared support files
provide reusable infrastructure and earlier assignment machinery; they do not implement the
new PA28 native lowering contract for you.

Unlike PA1-PA9, there is no external reference binary for PA28. The checked-in `.ref`
result files are the default oracle.

### Driver Surface For This Assignment

Required in PA28:

- `--help` / `-h`
- `-o <outfile>`
- `--dump-machine-ir <mirfile>`
- `--target <target>`

Not yet required here:

- separate compilation through `-c`
- link-map dumping
- the private exception/runtime ABI path

Those later pipeline surfaces are owned by the later `cppgm++` object,
compile/link, and host-EH assignments.

### Input / Command-Line Arguments

Behaviour is undefined unless the command-line arguments match one of:

    $ lowir2native --dump-machine-ir <mirfile> <srcfile1> <srcfile2> ... <srcfileN>
    $ lowir2native -o <outfile> <srcfile1> <srcfile2> ... <srcfileN>
    $ lowir2native --dump-machine-ir <mirfile> -o <outfile> <srcfile1> <srcfile2> ... <srcfileN>
    $ lowir2native --target <target> --dump-machine-ir <mirfile> <srcfile1> <srcfile2> ... <srcfileN>
    $ lowir2native --target <target> -o <outfile> <srcfile1> <srcfile2> ... <srcfileN>
    $ lowir2native --target <target> --dump-machine-ir <mirfile> -o <outfile> <srcfile1> <srcfile2> ... <srcfileN>

where each `<srcfileK>` is a LowIR source file and `<target>` is `linux`.

With no `--target`, `lowir2native` should emit a native Linux executable.

### Output Format

If `-o <outfile>` is provided, `lowir2native` shall write a native executable
program to `<outfile>`.

If `--dump-machine-ir <mirfile>` is provided, `lowir2native` shall also write a
deterministic machine-IR dump to `<mirfile>`.

The machine-IR dump is the serialized form of the backend model used for native
emission. You may keep a typed MIR internally, and the optional
`dev/src/mir_model.h` scaffold gives one possible representation, but the dump
must describe the same program that native emission consumes.

Frame metadata is part of that final MIR contract. In particular, the
callee-saved `preserve` list should name the callee-saved registers that the
final instruction body actually uses after local setup/copy cleanup, and the
stack size should match that final frame layout.

That MIR dump path must work even for helper-only LowIR inputs that have no
entry function. In that case the dumped MIR should simply omit the optional
`startup` section.

For the native path, that means an ELF executable.

The exact binary encoding is not directly compared by the PA28 tests. Instead, the tests
compare:

- the compiler exit status
- the canonical machine-IR oracle for successful compilations
- the generated program exit status
- the generated program standard output

### Error Handling

If an error occurs while parsing LowIR, validating LowIR, lowering LowIR, or writing the
native output, `lowir2native` shall `EXIT_FAILURE`.

The output file is not required to be meaningful on failure.

### Standard Output / Error

Standard output and standard error are ignored for automated testing of `lowir2native`.

You are free to use them for debugging, tracing, or diagnostic messages.

### Testing

Testing is based on execution of the generated native program.

For each test case `x`:

- `lowir2native` is executed to produce `x.my.program`
- `lowir2native` is also executed with `--dump-machine-ir` to produce `x.my.mir`
- the compiler exit status is recorded in `x.my.impl.exit_status`
- if compilation succeeded, `x.my.program` is executed
- its standard output is recorded in `x.my.program.stdout`
- its numeric exit status is recorded in `x.my.program.exit_status`

The checked-in `.ref` files are compared the same way for the outputs that are
part of that test's oracle:

- `x.ref.impl.exit_status`
- `x.ref.mir` for tests with a raw MIR dump oracle
- `x.ref.program.stdout`
- `x.ref.program.exit_status`

The `--dump-machine-ir` output remains the raw debugging dump.

For a successful compilation, the tested raw MIR dump is a plain-text file with this overall
shape:

```text
machine_ir x86_64 <target>

startup
    ...

global @name
  ...

function @name
  abi
    ...
  frame
    ...

  block ^label
    ...
```

The exact instruction inventory is target- and lowering-dependent, but the output format used
for testing is still this textual machine-IR form:

- one `machine_ir x86_64 <target>` header
- an optional `startup` section
- zero or more `global @...` definitions
- one or more `function @...` definitions
- per-function `abi`, `frame`, and ordered `block ^...` sections
- one instruction or metadata line per indented row beneath those sections

For strict and structural MIR tests, the raw `.ref.mir` file is still checked in because it
is the debugging-oriented dump students see directly from `--dump-machine-ir`.
Structural tests also keep `x.ref.cmir`, the canonical oracle used for grading.

For testing, PA28 uses three explicit comparison modes, split by directory:

1. `tests/strict/` compares the raw checked-in `.ref.mir` against the generated `.my.mir`,
   after only normalizing the host-target tag in the `machine_ir x86_64 <target>` header.
2. `tests/structural/` compares the checked-in `.ref.cmir` against a canonicalized form of
   the generated `.my.mir`.
3. `tests/behavior/` checks compilation and generated-program behavior only. It intentionally
   has no machine-IR oracle.

The structural canonicalization pass is intentionally conservative. It hides:

- the host-target tag in the MIR header
- exact stack/frame displacement numbers in memory operands
- interchangeable free GPR choices where the structural MIR shape is otherwise the same
- interchangeable free XMM choices where the structural MIR shape is otherwise the same

It still preserves:

- opcode family and width
- direct vs indirect call shape
- direct compare-to-branch vs materialized-bool shape
- register vs stack vs immediate location class
- floating operation family and explicit conversion family

So the assignment keeps a structural backend oracle without freezing exact
frame-layout details into every checked-in reference.

That means a successful `PA28` test anchor now validates exactly these output files:

- `x.ref.impl.exit_status`: exact compiler success/failure result
- `x.ref.program.exit_status`: exact generated-program exit status
- `x.ref.program.stdout`: exact generated-program standard output
- plus either:
  - `x.ref.mir` with strict raw-MIR comparison and header normalization only
  - `x.ref.mir` plus `x.ref.cmir`, with structural canonical-MIR comparison using
    checked-in `x.ref.cmir`
  - no MIR reference files for `tests/behavior/`

In other words, `PA28` is not just "program behavior matches." The tests also validate the
shape of the lowered backend output through one of those two explicit MIR oracles.

For structural failures, the harness leaves behind:

- `x.my.cmir`

Those are debugging artifacts only. Students are not expected to emit `.cmir` files. They
only need to implement `--dump-machine-ir` and produce raw `.mir`.

The `tests/behavior/` directory is for correctness cases where several reasonable
register-allocation or spill strategies are acceptable. Those tests still require
successful compilation and matching generated-program behavior, but they intentionally do
not compare a machine-IR oracle.

`make test` recursively runs the checked-in local suites:

- `tests/strict/`
- `tests/structural/`
- `tests/behavior/`

These directories contain PA28-specific backend oracle tests, not source-standard tests.
PA28 has no `tests/spec/` directory because the tested contract is the
compiler-owned LowIR-to-native backend surface rather than an N3485 C++ source-language
clause.

The PA28 suite is intentionally mixed:

- hand-written PA13-style LowIR tests
- selected LowIR programs copied from the outputs of PA15-PA27

That ensures PA28 is tested both on the core LowIR forms and on the richer LowIR that later
lowering assignments now produce.

The PA28 test suite exercises:

- startup/lowering correctness for simple programs, globals, direct calls, and indirect calls
- register and stack calling-convention handling for:
  - integer-only calls
  - mixed GPR/XMM direct calls
  - mixed GPR/XMM indirect calls
- short-circuit-style branch diamonds expressed directly in LowIR control flow
- unary logical-not lowering when the result feeds control flow
- direct compare-fed branches over integer, pointer, and floating inputs
- compare-as-value materialization for integer, pointer, and floating cases
- trivial integer and floating leaf chains that should stay register-resident
- mixed integer/float conversion chains
- signed and unsigned narrow integer reload/widen paths from both frame and global storage
- conservative `f80` arithmetic, comparison, and call/data lowering
- atomic load/store, exchange, compare-exchange, fetch-add, and fence operations across
  multiple scalar widths

The shipped PA28 tests are the contract for this milestone.

### PA28 Syntax Spec

The authoritative input-language syntax for PA28 is `pa28.gram`.

As in the earlier assignments, that grammar defines accepted input syntax only. The native
program behaviour contract is specified by this README, PA13 `lowir.md`, and the checked-in
`.ref` files.

PA28 does not add new LowIR syntax beyond PA13. It reuses the same LowIR language and adds
a new backend target for it.

That means the expected PA28 input surface is the current PA13 LowIR surface, not the older
pre-metadata subset. In particular, handwritten PA28 inputs may now use:

- explicit function role metadata such as `[role=entry]`, `[role=init]`, and `[role=fini]`
- top-level declaration forms such as `declare function` and `declare global`
- structured global data plus explicit global storage metadata where relevant
- optional call-boundary, parameter, and instruction-debug metadata accepted by PA13

A checked-in HTML grammar explorer for that grammar lives in `grammar/`. Treat
`pa28.gram` as the source of truth.

If this README and `pa28.gram` appear to disagree about LowIR syntax, treat `pa28.gram` as
authoritative. If this README and `../pa13/lowir.md` appear to disagree about the full LowIR
definition, treat `lowir.md` as authoritative. If they disagree about the required PA28
implementation subset, treat the `Assignment Boundary` and `Out Of Scope` sections below as
authoritative.

### Assignment Boundary

PA28 must support native lowering for the LowIR family already defined for PA13 and used by
PA14-PA27, including:

- scalar globals and structured global data
- functions, blocks, slots, temporaries, and runtime hooks
- direct and indirect calls
- control flow, integer operations, and pointer/index operations
- floating scalar operations and comparisons over `f32`, `f64`, and `f80`
- explicit scalar conversions:
  - `sitofp`
  - `uitofp`
  - `fptosi`
  - `fptoui`
  - `fpext`
  - `fptrunc`
- atomic scalar operations and fences over `i1`, `i8`, `i16`, `i32`, `i64`, and `ptr`
- bulk memory operations:
  - `copyobj`
  - `zeroinit`
- object-lowered ABI forms emitted by source-to-LowIR assignments:
  - hidden destination-pointer returns
  - lowered object parameters carried as `ptr`
- structured vtable/global table data emitted by source-to-LowIR lowering

Within this milestone, PA28 should successfully compile the LowIR emitted by PA14-PA27 into
host-native executables, without requiring CY86 as the primary output format.

PA28 must also expose a deterministic machine IR for successful compilations. That dump is
the structural proof that lowering is happening directly from LowIR into a target-specific
backend representation rather than only through a CY86 scaffold.

The LowIR input path should parse the same LowIR text accepted by PA13 rather
than relying on a private object, semantic, or source-level backchannel. Any
backend fact needed below PA28 belongs either in LowIR text or in the
target-specific MIR produced from that LowIR.

Within the supported subset, PA28 should lower:

- direct function calls to direct machine-IR call sites
- block control flow to direct machine-IR conditional and unconditional branches
- startup and shutdown hooks to direct machine-IR call sites in the startup path
- bulk object-memory operations to first-class machine-IR `copy_bytes` / `zero_bytes`
  instructions
- truly indirect LowIR calls to machine-IR indirect calls, rather than forcing all calls
  through the same lowered shape
- structured global data to machine-IR global data blocks rather than flattening everything
  through a CY86-style scalarized path
- atomic scalar LowIR to first-class machine behavior rather than silently dropping the
  atomic contract in the direct backend
- the LowIR arithmetic and conversion forms needed for native execution parity on the
  backend-owned subset of the old PA9 execution envelope, especially:
  - signed/unsigned integer division, modulus, ordered comparisons, and right shift
  - integer/float conversion operations
  - float-width extension and truncation operations
  - `f32`/`f64`/`f80` arithmetic and comparison behavior

To complete PA28, implement these goals:

1. Direct control-flow lowering.
   LowIR branches, first-class `switch` dispatch, and direct calls should become
   first-class machine-IR branches and direct calls, not a normalized CY86-style
   fallback.

2. Direct startup/runtime wiring.
   The startup path should call `@__cppgm_init`, `@main`, and `@__cppgm_fini` as direct
   machine-IR call sites where those hooks exist.

3. First-class bulk object-memory lowering.
   `copyobj <bytes>x<align>` and `zeroinit <bytes>x<align>` should survive as meaningful
   machine-IR operations such as `copy_bytes <bytes>x<align>` and
   `zero_bytes <bytes>x<align>`, rather than being expanded only through the old CY86
   lowering path.

4. Preserve the distinction between direct and indirect calls.
   The direct backend should still emit indirect machine-IR calls for truly indirect LowIR
   calls, such as virtual dispatch, instead of collapsing all calls into one lowered form.
   That includes pointer-valued global cells: if a call target comes from a scalar `ptr`
   global, PA28 should call through the pointer stored in that global, not through the
   address of the global storage itself.

5. Preserve richer LowIR data layout.
   Structured global data and later vtable-like globals should remain structured in the
   direct backend rather than being forced through a scalarized compatibility path.

6. Exercise backend-owned execution behavior directly.
   PA28 is the right home for LowIR-native execution tests that validate the basic machine
   semantics inherited from PA9 without waiting for the later source-driver/toolchain
   milestones. The important cases are arithmetic, signedness-sensitive integer behavior,
   scalar conversions, and floating execution. Those tests should be expressed in LowIR,
   not by reintroducing CY86 or a host-lib-dependent source harness.

   In particular, PA28 should already treat unsigned LowIR arithmetic/predicate forms such as
   `udiv`, `umod`, `ushr`, `ult`, `ule`, `ugt`, and `uge` as first-class backend behavior,
   not as optional later cleanups.

7. Preserve direct compare-fed branch lowering for ordinary scalar cases.
   When a compare result feeds exactly one branch, PA28 should lower that as a direct
   machine compare plus conditional branch rather than materializing a boolean temporary
   and branching on that temporary afterward.

8. Keep simple scalar and floating work on the appropriate machine path.
   Small leaf scalar expressions should normally stay in registers, and ordinary `f32` /
   `f64` operations should stay on the floating-register path. A conservative stack spill
   is acceptable when pressure or an ABI boundary requires it, as long as the generated
   program is correct and the checked structural MIR cases still match their oracles.

9. Implement call-boundary correctness without requiring a clever allocator.
   PA28 must respect the native calling convention for direct calls, indirect calls,
   mixed GPR/XMM arguments, stack arguments, returned values, and values that remain live
   across calls. The tests intentionally check some high-pressure call cases by program
   behaviour only; those cases should compile and run correctly but do not require the
   exact spill/register strategy used by the reference implementation.

10. Keep mixed-width conversion and floating-bool materialization explicit.
   Mixed integer/float conversion chains should keep their conversion family and width
   visible in MIR, and floating compare results used as values may materialize booleans
   in registers without an unnecessary stack round-trip.

11. Preserve narrow integer width behavior in MIR.
   Ordinary `i8`/`u16` compare-fed branches should stay visibly narrow, and small signed
   or unsigned integer arithmetic should show the expected post-operation normalization
   instead of silently widening into an untyped 64-bit path.

12. Keep the conservative `f80` path explicit rather than implicit.
   PA28 does not need to treat `f80` like ordinary XMM-resident `f32`/`f64`, but its
   conversions and truncation/extension path should still stay visible and testable in
   MIR.

13. Cover direct compare-fed branch lowering at ordinary 64-bit integer width too.
   The direct compare/branch quality rule is not limited to `i32` and `u32`. PA28 should
   also show the same direct branch shape for straightforward `i64` comparisons. The core
   oracle for this is the `500-i64-direct-compare-branch` family.

14. Keep pointer/null comparisons on the direct machine compare/branch path.
   Ordinary pointer/null tests should remain visibly pointer-typed in MIR and branch
   directly rather than degrading into a less explicit scalarized path, including
   null values first introduced through `const ptr 0`.

15. Keep pointer/index address calculation visible as pointer arithmetic.
   Pointer indexing and pointer-difference behavior should stay structurally visible in MIR
   rather than being hidden behind an unrelated compatibility path.

16. Preserve mixed integer/floating call ABI classification.
   Calls that mix GPR and XMM arguments should keep that classification visible in MIR so
   students can tell whether the backend is respecting the native calling convention.

17. Keep ordinary `f80` arithmetic and comparison behavior executable and visible.
   Even though `f80` remains the conservative floating special case, simple `f80`
   arithmetic and `cmp` behavior should still run correctly and remain explicit in MIR.

18. Exercise non-64-bit atomic widths explicitly.
   The PA28 atomic contract is not only about `i64`; smaller-width atomic load/store
   behavior should survive through the direct native backend.

The PA28 tests intentionally include all of those cases so students can tell whether they
have actually implemented a direct `LowIR -> machine IR -> native` path, rather than only
matching program behaviour through a hidden CY86-based route.

### Out Of Scope

The following are explicitly out of scope for PA28:

- separate compilation and linking
- relocatable object-file output
- exception-aware native runtime metadata
- optimization passes
- non-x86 native instruction selection
- source-level end-to-end runtime programs that depend on the later `cppgm++ -c`
  driver and object/library flow

Inputs that rely on those features have undefined behaviour for this milestone.

### Stage Handoff

The intended next stages are:

- PA31 host EH facts, which validates the host-EH metadata emitted in
  `cppgm++ -c` objects
- PA29 `cppgm++` compile/link mode, which adds source-driven separate
  compilation and linking on top of the native backend path

So PA28 should leave behind:

- a stable `LowIR -> native` lowering boundary
- a stable `LowIR -> machine IR` boundary that later optimization passes can target
- reusable target-specific code/data emission layers
- no renewed dependence on CY86 for the main compiler path
- a backend test corpus that already catches the basic execution-level
  arithmetic and conversion bugs before the source-driven toolchain stages

### Design Notes (Non-Normative)

The cleanest PA28 structure is:

- parse LowIR into a structured internal representation
- lower that representation into a structured machine-IR program
- dump that machine-IR program deterministically for testing
- lower that machine-IR program into target-specific code/data
- write the final executable image from that lowered form

The important architectural constraint is that PA28 should reuse PA9 knowledge without
re-coupling the compiler to CY86. CY86 may remain useful as a secondary validation path, but
the primary backend boundary should now be LowIR to machine IR and native code/data.
