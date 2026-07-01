## CPPGM Programming Assignment 29 (`cppgm++ -c` and link mode)

### Overview

Write one C++ application called `cppgm++`.

PA29 does not introduce a new executable. It extends the same `cppgm++` binary
used since PA10 with practical compiler-driver behavior.

`cppgm++` has two required PA29 modes:

- compile mode, `-c`, which takes one C++ source file and writes one
  implementation-defined compiler object file
- default link mode, which takes one or more inputs and writes one native
  executable program

In link mode, each input may be either:

- a C++ source file, which `cppgm++` compiles as its own translation unit before
  linking
- a compiler object file previously produced by `cppgm++ -c`

The contract is source-driven. The PA29 tests start from C++ source
files, validate explicit separate compilation with `cppgm++ -c`, and then link
the resulting objects with `cppgm++`. The harness also checks two practical
driver consistency properties:

- linking the same source files directly through `cppgm++` must match explicit
  compile-then-link behavior
- linking a mixture of precompiled objects and remaining source files must also
  match explicit compile-then-link behavior

PA29 does not introduce a new language subset. It turns the C++ feature set
implemented through PA27 into a practical compile-and-link toolchain entrypoint.

### Prerequisites

Complete PA28 before starting this assignment.

You will want to reuse:

- the preprocessing and tokenization pipeline from PA1-PA6
- the PA10 AST and PA11/PA12 semantic foundation
- the PA14-PA27 LowIR lowering path
- the PA28 native backend
- the object emission, linking, and runtime support path used by `cppgm++`

The tests assume a POSIX-like shell environment with `make`, `bash`,
`perl`, and a working host C/C++ compiler for test helper objects. The harness
selects helper compilers from:

- `CPPGM_HOST_CC` or `CC` for C helper sources
- `CPPGM_HOST_CXX` or `CXX` for C++ helper sources

If those are not set, the harness searches for common compilers such as
`clang`, `gcc`, `cc`, `clang++`, `g++`, and `c++`. Some tests substitute the
Linux target name or the corresponding x86_64 Linux triple,
`x86_64-unknown-linux-gnu`, into driver flags.

### Starter Kit

The starter kit provides:

- `dev/cppgm++.cpp`, populated from the `cppgm++` scaffold for the cumulative
  PA10+ compiler driver
- the shared `dev/` sources needed by the scaffold
- `pa29/cppgm++.cpp`, a link to `../dev/cppgm++.cpp`
- `pa29/Makefile`
- `pa29/scripts/`, the compiler-driver test harness
- `pa29/tests/general/`, the PA29 tests and checked-in reference files
- the shared `cppgm++` source grammar, exposed for this assignment as
  `pa29.gram`
- an HTML grammar explorer of `pa29.gram` in the sub-directory `grammar/`

Student code changes should go in `dev/`, especially `dev/cppgm++.cpp` and the
shared implementation files it calls. Do not edit generated `.my` files. Test
inputs and references are part of the handout unless your instructor asks you
to add or update tests.

There is no separate PA29 reference binary in the starter kit. The checked-in
`.ref.*` files are the oracle.

### Driver Surface

Previously required:

- `--emit-ast`
- `--emit-types`
- `--emit-semantics`
- `--emit-lowir`
- `-o <outfile>`

New in PA29:

- compile mode: `-c`
- default link mode with source and object inputs
- include search: `-I <dir>` and `-I<dir>`
- library search: `-L <dir>`, `-L<dir>`, `-l <name>`, and `-l<name>`
- target selection: `--target <target>` or `--target=<target>`

Not yet required here:

- hosted preprocess mode `-E`
- hosted preprocessor-control flags such as `-D`, `-U`, `-include`, and
  `-isystem`
- driver query flags such as `--version`, `-v`, `-dumpmachine`,
  `-dumpversion`, and `-print-search-dirs`
- static archives and shared libraries as link inputs

### Command-Line Contract

Required compile forms:

```sh
cppgm++ -c -o <objfile> <srcfile>
cppgm++ -c --target <target> -o <objfile> <srcfile>
cppgm++ -c -I <dir> -o <objfile> <srcfile>
cppgm++ -c -I<dir> -o <objfile> <srcfile>
cppgm++ -c --target <target> -I <dir> -o <objfile> <srcfile>
cppgm++ -c --target <target> -I<dir> -o <objfile> <srcfile>
```

Required link forms:

```sh
cppgm++ -o <outfile> <input1> <input2> ... <inputN>
cppgm++ --target <target> -o <outfile> <input1> <input2> ... <inputN>
cppgm++ -I <dir> -o <outfile> <input1> <input2> ... <inputN>
cppgm++ -I<dir> -o <outfile> <input1> <input2> ... <inputN>
cppgm++ -L <dir> -l<name> -o <outfile> <input1> <input2> ... <inputN>
cppgm++ -L<dir> -l <name> -o <outfile> <input1> <input2> ... <inputN>
```

Options may be combined when their meanings are compatible, for example
`--target <target>` with `-I` or `-L`/`-l`.

In link mode, each `<inputK>` may be:

- a C++ source file
- an object-like file produced by `cppgm++ -c`

For PA29, object files are identified by implementation-supported object-like
filenames such as `.o` or `.obj`. The checked-in tests use `.obj`.

`-I` adds user include search paths for any C++ source files compiled in that
invocation. The tests use `-I` with quoted includes.

`-L` and `-l` search implementation-supported object-like libraries. The tests use simple helper objects named like `lib<name>.o` in a harness-created
library directory.

All linked inputs in one invocation must target the same native backend target.

### Output Format

In compile mode, `cppgm++` shall write one compiler object file to `<objfile>`.

In link mode, `cppgm++` shall write one native executable program to
`<outfile>`.

The PA29 object-file encoding is intentionally an internal `cppgm++` contract:
the file must be consumable by `cppgm++` link mode, but it does not need to be
accepted by the host linker. The exact object-file encoding is not directly
compared by the PA29 tests. The exact final binary encoding is also not
directly compared. Instead, the tests compare:

- compile/link exit status
- generated program exit status
- generated program standard output

### Error Handling

If an error occurs during preprocessing, parsing, semantic analysis, lowering,
object-file emission, linking, or native output writing, `cppgm++` shall exit
with failure.

Important PA29 error cases include:

- duplicate global symbol definitions
- unresolved external symbols
- missing `main`

For negative tests, exact diagnostics are not the grading contract. The harness
compares exit status first. If the reference compile/link path fails, stdout and
stderr are diagnostic side effects rather than required output.

### Standard Output And Error

Standard output and standard error are ignored for successful automated testing
of `cppgm++` in PA29. You may use them for diagnostics.

### Testing

Run the PA29 suite with:

```sh
make test
```

To run one test through the shared check target:

```sh
make check TEST=tests/general/100-two-source-call.t
```

The local tests live in `tests/general/`. They exercise practical
compiler-driver, separate-compilation, link, runtime, and consistency behavior.
They are not direct N3485 clause tests.

For each test anchor `x.t`, companion C++ sources are named:

```text
x.t.1
x.t.2
...
```

Optional sidecars include:

- `x.flags`: extra flags passed to `cppgm++`
- `x.lib.*`: host-built helper C or C++ sources that become object-like
  libraries for `-L`/`-l` tests
- `x.stdin`: standard input for the generated program

For each test case, the harness checks:

1. Explicit separate compilation:
   `cppgm++ -c` is executed once for each companion source file, and then
   `cppgm++` links the generated objects.
2. Direct source linking:
   `cppgm++` is executed directly on the same source files.
3. Mixed source/object linking for multi-source tests:
   one generated object and the remaining source files are linked together.

The checked-in `.ref.*` files are compared against the explicit compile/link
path. The direct and mixed paths are consistency checks: they must match the
explicit path.

This validates:

- compile mode
- link mode
- source-to-object lowering through the full language pipeline
- consistency between direct source linking and explicit separate compilation
- consistency between mixed source/object linking and explicit separate
  compilation
- cross-translation-unit data relocations that feed indirect calls
- namespace-scope startup hooks across translation units

### Assignment Boundary

PA29 must support the C++ feature set already implemented through PA27, but
through a practical driver interface rather than one stage-specific binary per
milestone.

Within that supported subset, PA29 should:

- compile one C++ source file to one compiler object file with `-c`
- link compiler object files into a native executable
- accept C++ source files directly in link mode by compiling each source as its
  own translation unit before linking
- support user include search paths through `-I`
- support source-level external declarations needed for ordinary separate
  compilation, such as `extern int g;`
- support ordinary external C function declarations and definitions through
  `extern "C"` in the practical subset needed for object-style library
  interoperability
- support object-like library search through `-L` and `-l`
- support simple complete-program runtime tests written in C++ and linked
  against harness-provided object-style support libraries, without requiring
  host libc or hosted headers
- allow an implementation-defined compiler object format with your own linker
  for PA29, as long as the `cppgm++` behavior matches the contract

To complete PA29, implement these goals:

1. Separate compilation from C++ source.
2. Direct source-link parity.
3. Mixed source/object parity.
4. Cross-translation-unit source semantics.
5. Toolchain-style include handling.
6. External object-library interoperability through the tested `extern "C"`
   and `-L`/`-l` subset.
7. Full-language-through-toolchain validation for previously implemented
   language features.
8. Source-driven runtime-program validation without host-library dependence.

### Out Of Scope

The following are out of scope for PA29:

- full system-compiler flag compatibility beyond the documented PA29 options
- static archives such as `.a`
- shared libraries such as `.so` or `.dylib`
- arbitrary foreign non-object library formats
- full `extern "C"` linkage-specification coverage beyond the practical
  function-oriented subset needed for PA29 interop
- dependence on host libc or hosted headers for the basic PA29 runtime-program
  coverage
- dependency generation flags
- precompiled headers
- build-system conveniences such as depfiles or compilation databases
- hosted preprocessor and hosted-header compatibility, which belong in PA34
  and PA36
- standalone ABI name construction, which belongs in PA30
- host-linker-compatible object output, which belongs in PA31/PA32

### Design Notes (Non-Normative)

PA29 should wrap the existing implemented pipeline, not replace it.

In particular:

- C++ source inputs should still flow through the existing semantic and LowIR
  lowering path.
- The object and link stages should still reuse the object/runtime machinery
  from earlier assignments.
- The direct source-link path should behave like repeated separate compilation
  followed by linking, not like a special one-off shortcut.
- Do not carry a private PA29 object encoding forward as the host-object
  solution. PA31/PA32 replace the internal compiler-object contract with a
  host-linker-compatible object contract.

### Stage Handoff

The next stage is PA30, which isolates Itanium C++ ABI name construction before
the later host-object assignments require host-compatible C++ symbol names.
