## CPPGM Programming Assignment 13 (`lowir2cy86`)

### Overview

Write a C++ application called `lowir2cy86`:

```sh
lowir2cy86 -o <outfile> <srcfile1> [<srcfile2> ...]
```

The program reads one or more LowIR source files, validates the PA13 LowIR
subset, and writes equivalent PA9 CY86 source text to `<outfile>`.

PA13 does not lower C++ source. Its job is to establish a runnable backend
adapter for the LowIR text format. Later assignments lower C++ into LowIR and
can use this adapter for execution checks without making CY86 the compiler's
main intermediate language.

### Prerequisites

Complete PA9 before starting PA13. You should expect to reuse:

- the PA9 CY86 source language and execution model
- the PA9 code-generation and runtime-testing model
- any parser infrastructure from earlier assignments that is useful for a
  line-oriented IR format

PA10-PA12 are not required for the core PA13 implementation. PA13 is a backend
adapter over LowIR text.

### Starter Kit

The PA13 starter kit contains:

- `README.md`, this assignment handout
- `Makefile`, which builds `lowir2cy86` and runs the PA13 tests
- `lowir2cy86.cpp`, a link to the editable `dev/lowir2cy86.cpp` entry point
- the grammar for this assignment called `pa13.gram`
- `lowir.md`, the LowIR format reference for this assignment family
- optional typed LowIR model scaffolding in `dev/src/lowir_model.h` with
  shared exported-symbol support in `dev/src/ir_symbol_model.h`
- an HTML grammar explorer of `pa13.gram` in the sub-directory `grammar/`
- `scripts/run_all_tests.pl` and `scripts/compare_results.pl`
- `tests/spec/`, the LowIR-to-CY86 tests
- checked-in `.ref` and `.ref.exit_status` files used as the oracle

Your main editable file is `dev/lowir2cy86.cpp`. You may add or change other
implementation files under `dev/` as needed. Do not edit the test inputs,
reference outputs, harness scripts, grammar file, or LowIR specification unless
course staff explicitly asks for that.

The starter `dev/lowir2cy86.cpp` is a command-line scaffold. It parses
`-o <outfile> <srcfile...>` and leaves LowIR parsing, validation, and CY86
translation as the assignment work.

There is no required `lowir2cy86-ref` binary for PA13. The checked-in
reference files under `tests/spec/` are the grading oracle.

### Build And Test Commands

From the `pa13/` directory:

```sh
make
make test
```

`make` builds `lowir2cy86`. `make test` runs the LowIR-to-CY86 suite
under `tests/spec/`. If a `course/pa13` extension suite is present, the
Makefile runs it after the local suite using the same harness contract.

### Required Driver Surface

PA13 requires:

- `-o <outfile>`
- one or more LowIR source-file operands

Behavior is undefined unless the command line has this form:

```sh
lowir2cy86 -o <outfile> <srcfile1> [<srcfile2> ...]
```

The following are not part of the PA13 `lowir2cy86` driver contract:

- native object or executable emission
- `lowir2native`
- `lowiropt`
- `--dump-machine-ir`
- `--target <target>`
- link-map dumping
- object-file debug-info or debugger integration

### Input Contract

Each input file is a LowIR source file. Multiple files are processed in
command-line order as one LowIR program.

The authoritative input syntax is `pa13.gram`. The format reference is
`lowir.md`. If this README and `pa13.gram` disagree about input syntax, use
`pa13.gram`. If this README and `lowir.md` disagree about the LowIR text
format, use `lowir.md`. If they disagree about what PA13 must implement, use
the "PA13 LowIR Contract" and "Out Of Scope" sections below.

Behavior is undefined for input that:

- is not syntactically valid LowIR
- is structurally malformed in a way this assignment requires you to reject
- uses LowIR features outside the PA13 implementation contract

### Output Format

On success, `lowir2cy86` writes CY86 source text to `<outfile>`.

The generated CY86 must follow the PA9 source-language contract. The exact text
is deterministic and is compared against the checked-in `.ref` files for
successful tests.

The PA13 oracle is the generated CY86 text and exit status. Running the CY86
through the PA9 `cy86` tool is useful manual validation, but the committed PA13
tests compare the CY86 text directly.

Standard output and standard error are ignored by the automated PA13 tests.

### Error Handling

If LowIR parsing, validation, or translation fails, `lowir2cy86` must exit with
`EXIT_FAILURE`.

The contents of `<outfile>` are unspecified on failure. For failing tests, the
harness compares only the named exit status, not diagnostic text and not the
output file.

### PA13 LowIR Contract

PA13 must parse, validate, and translate the LowIR features needed by the `tests/spec/` suite and described in `lowir.md`.

Required program structure:

- top-level `declare global`, `declare function`, `global`, `function`, and
  `alias object` forms
- scalar and structured global definitions
- function definitions with parameters, stack slots, blocks, and instructions
- one entry function, identified by `[role=entry]` or the legacy `@main`
  spelling
- optional init and fini hooks, identified by `[role=init]` / `[role=fini]` or
  the legacy `@__cppgm_init` / `@__cppgm_fini` spellings

Required types:

- `void`
- `i1`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`
- `f32`, `f64`, and `f80`
- `ptr`
- restricted direct object boundary types such as `obj<8x4>`

Required metadata families:

- top-level `role`, `linkage`, `binding`, `object`, function `tls_for`,
  `keep_alias`, `prefer_local`, and global `storage`
- function `arity`, `effects`, `unwind`, and `return`
- parameter `pass`, `capture`, `access`, and `alias`
- index `projection`

The metadata is part of the textual LowIR contract because later compiler
stages must be able to preserve semantic call-boundary and symbol-boundary
facts in LowIR text. PA13 translates those facts only to the extent needed for
the CY86 adapter. It does not implement native object symbol binding, host ABI
register assignment, or debugger behavior.

You may keep a typed LowIR model internally, and the optional
`dev/src/lowir_model.h` scaffold names the common program, symbol, type,
operand, block, and instruction pieces. The typed model is support for the
text format, not a replacement for it: if a later backend or object writer
needs a fact, that fact must be representable in serialized LowIR text and
recoverable by parsing that text back in.

Required instructions:

- `const`, `copy`, `addr`, `load`, and `store`
- `atomic_load`, `atomic_store`, `atomic_exchange`,
  `atomic_compare_exchange`, `atomic_add_fetch`, `atomic_thread_fence`, and
  `atomic_signal_fence`
- `index`
- `copyobj` and `zeroinit`
- `unary`, `binary`, `cmp`, and `convert`
- direct and indirect `call`, including explicit `as (...) -> ...` signatures
  for indirect calls
- `jump`, `branch`, `switch`, and `return`
- the exception/runtime LowIR forms listed in `lowir.md` when they appear in
  the PA13 tests: `eh_try`, `eh_cleanup`, `eh_end`, `throw`,
  `exception`, and `resume`

For `cmp`, the type written in the instruction is the operand comparison type,
not the result type. For example, `%ok = cmp eq f80 %a, %b` compares two `f80`
operands, but `%ok` is an integer truth value. PA13 adapters should materialize
all `cmp` results as canonical `i64` values, `0` for false and `1` for true,
including `f32`, `f64`, `f80`, pointer, and narrow integer comparisons. A later
`branch %ok` or `return i64 %ok` should be able to consume that result directly.

For the PA13 CY86 adapter, atomic operations may use a course-defined
single-threaded interpretation:

- atomic loads and stores behave like ordinary loads and stores
- atomic exchange returns the previous value and stores the new value
- atomic compare-exchange returns `1` on success, returns `0` on failure, and
  updates the expected-value storage on failure
- atomic add-fetch returns the updated value
- fences are accepted and may lower to no code

For `f80`, preserve the LowIR-facing storage size and alignment described in
`lowir.md`. If your adapter does not have a native 80-bit calling convention,
you may use an implementation strategy that still preserves the LowIR
source contract and produces the expected CY86 behavior.

### Structural Validation

Reject structurally malformed LowIR, including:

- duplicate top-level symbol names
- duplicate object alias spellings
- an object alias whose target is not a top-level declaration or definition
- a `tls_for` wrapper whose target is not a thread-local global
- duplicate parameter, slot, or block names inside one function
- a function with no blocks
- a block with no terminator
- instructions after a terminator in the same block
- branch, jump, or switch targets that do not name a block in the same function
- undefined temporaries, slots, globals, functions, or blocks where PA13
  requires a definition
- invalid metadata values
- parameter metadata that is not legal for the parameter type
- `indirect_result` parameters that are not first or are used on non-`void`
  functions
- indirect calls that omit the required explicit signature

Diagnostics are not graded, but the exit status is.

### LowIR Family Context

`lowir.md` also describes LowIR facts that become important for later
assignments, including object ABI metadata, exception/runtime roles, optional
debug-location text, and future reserved extensions. This material gives the
LowIR family a stable direction, but PA13 remains the `lowir2cy86` adapter.

For PA13:

- Accept and translate later-facing LowIR forms only when this README,
  `lowir.md`, and the `tests/spec/` suite make them part of the PA13
  adapter contract.
- Treat `!dbg(...)` as LowIR text metadata. Generating native object-file debug
  information, validating DWARF dump output, and running debugger checks are
  outside PA13.
- Do not implement native code generation, linking, hosted runtime behavior,
  LowIR optimization, or C++ source-to-LowIR lowering as part of PA13.

### Out Of Scope

PA13 does not require:

- lowering C++ source into LowIR
- LowIR optimization passes
- direct native code generation
- object-file emission
- linking
- host ABI register assignment
- object-file debug-info emission
- source-language object construction or destruction semantics
- member/field access operations as a distinct LowIR instruction family
- virtual-call or vtable-specific operations

Inputs that rely on unassigned features have undefined behavior for PA13 unless
they are explicitly covered by the PA13 LowIR contract above.

### Testing And Grading Contract

The PA13 harness discovers every `.t` file under `tests/spec/`.
For each test case `x.t`, it runs:

```sh
lowir2cy86 -o x.my x.t
```

and records `x.my.exit_status`.

Comparison rules:

- `x.my.exit_status` must match `x.ref.exit_status`.
- If the reference status is `EXIT_FAILURE`, the test passes after the exit
  status comparison.
- If the reference status is `EXIT_SUCCESS`, `x.my` must match `x.ref` exactly.
- Standard output and standard error are not compared.

PA13 does not use the relaxed source-to-LowIR matcher used by later
`cppgm++ --emit-lowir` assignments. PA13 compares generated CY86 text directly.

### Design Notes (Non-Normative)

A robust `lowir2cy86` design usually has three layers:

- parse LowIR into a structured program representation
- validate symbols, types, blocks, metadata, and instruction constraints
- translate the validated representation mechanically into CY86

Keep the translation monotonic. Adding a new LowIR instruction later should add
a translation case without changing the CY86 output for existing PA13 programs.
