## CPPGM Programming Assignment 34 (`cppgm++ -E` / `cppgm++ -c`)

### Overview

PA34 is the hosted source/header compatibility assignment. Its job is to make
`cppgm++` preprocess and compile the hosted standard-library and vendor
extension environment needed by later bootstrap-style builds.

This milestone is intentionally distinct from the previous host-toolchain
assignments:

- PA32 is about ordinary host-linkable object files.
- PA33 is about host C++ ABI/runtime correctness after host link.
- PA34 is about preprocessing, parsing, semantic analysis, and lowering
  compatibility for hosted source/header inputs.

To complete PA34, implement these goals:

- hosted preprocessor compatibility
- GNU/Clang parser concessions used by the selected hosted headers
- GNU builtin type and literal forms used by the selected hosted environment
- builtin traits, transforms, and intrinsics used by that hosted environment
- enough hosted-header/source compile compatibility to make later bootstrap
  work realistic (the heaviest standard-library headers are deferred to PA35)

### Prerequisites

Complete PA33 before starting this assignment.

You will want to reuse:

- the full earlier language, template, and lowering stack
- the PA32/PA33 `cppgm++ -c` host-object path
- the PA33 host-ABI-compatible output path
- the earlier preprocessor pipeline, now with hosted-driver controls

The tests assume a Linux shell environment with `make`, `bash`, `perl`, and a
working host C++ compiler with hosted C++ headers installed.
You may override the compiler with `CXX=...`. `CPPGM_HOST_CXX` selects the host
compiler used for builtin macro/include probing. If it is not set, it defaults
to `CXX`.

The hosted tests rely on the host compiler's target, predefined macros, standard
library include paths, and standard-library selection flags. When you use a
non-default standard library, pass the same choice through `CPPGM_STDLIB_FLAGS`
so the course compiler and host compiler agree.

### Starter Kit

The starter kit provides:

- `dev/cppgm++.cpp`, populated from the `cppgm++` scaffold for the cumulative
  PA10+ compiler driver
- the shared `dev/` sources needed by the scaffold
- `pa34/cppgm++.cpp`, a link to `../dev/cppgm++.cpp`
- `pa34/Makefile`
- `pa34/scripts/`, the hosted preprocessor/compile test harness
- `pa34/tests/preproc/`, hosted preprocessor tests and references
- `pa34/tests/compile/`, hosted compile-only tests and references

Student code changes should go in `dev/`, especially `dev/cppgm++.cpp` and the
shared implementation files it calls. Do not edit generated `.my` files. Test
inputs and references are part of the handout unless your instructor asks you
to add or update tests.

There is no separate PA34 reference binary in the starter kit. The checked-in
`.ref.*` files are the oracle.

### Driver Surface

Previously required:

- the PA29 compile/link surface: `-c`, default link mode, `-I`, `-L`, `-l`, and
  `--target`
- the PA32/PA33 host-compatible behavior for the relevant compile-mode subset

New or newly required in PA34:

- hosted preprocess mode: `-E`
- hosted preprocessor-control flags:
  - `-D <macro>` and `-D<macro>`
  - `-U <macro>` and `-U<macro>`
  - `-include <file>`
  - `-isystem <dir>` and `-isystem<dir>`
- direct driver query forms:
  - `--version`
  - `-v`
  - `-dumpmachine`
  - `-dumpversion`
  - `-print-search-dirs`
- compatibility handling for common build-system flags that should either be
  honored or harmlessly accepted when they do not affect the tested output

### Command-Line Contract

PA34 continues extending the same `cppgm++` frontend used in PA30-PA33.

Required preprocess forms:

```sh
cppgm++ -E -o <outfile> <srcfile>
cppgm++ -E <srcfile1> <srcfile2> ...
cppgm++ -E -D <macro> -U <macro> -include <file> -isystem <dir> <srcfile>
```

Required compile forms:

```sh
cppgm++ -c -o <objfile> <srcfile>
cppgm++ -c <srcfile1> <srcfile2> ...
cppgm++ -c -D <macro> -U <macro> -include <file> -isystem <dir> -o <objfile> <srcfile>
```

Query flags are required only as direct driver queries. For example:

```sh
cppgm++ --version
cppgm++ -dumpmachine
cppgm++ -print-search-dirs
```

### Output Format

`cppgm++ -E` shall write the same structured posttoken/preprocessor stream
format used by the PA5 `preproc` frontend. When `-o <outfile>` is present, that
stream is written to `<outfile>`.

`cppgm++ -c` shall continue to write host-linker-compatible relocatable object
files as in PA32/PA33.

The new PA34 contract is not a new object format. It is the ability to
preprocess and compile hosted source/header inputs successfully through the
`cppgm++` path.

Hosted compatibility should use the same source-to-LowIR-to-object pipeline as
ordinary compilation. Hosted include search, predefined macros, and builtin
probes can affect the source program being compiled, but `--emit-lowir` should
remain representative of the LowIR that object emission consumes. Do not add a
hosted-only lowering path that carries backend facts outside serialized LowIR.

### Error Handling

If preprocessing, parsing, semantic analysis, lowering, object emission, or
output writing fails, `cppgm++` shall exit with failure.

For compile-only tests, exact diagnostics are not the grading contract. The
harness compares exit status and any checked output sidecars. If the reference
run fails, stdout and stderr are diagnostic side effects rather than required
output.

### Testing

Run the PA34 suite with:

```sh
make test
```

To run one test through the shared check target:

```sh
make check TEST=tests/preproc/300-has-include.t
make check TEST=tests/compile/600-builtin-transforms-and-traits.t
```

PA34 has two test directories:

- `tests/preproc/`: hosted preprocessor compatibility. The oracle is the
  PA5-style structured preprocessor stream plus exit status.
- `tests/compile/`: hosted compile-only compatibility. The oracle is successful
  object emission, compile exit status, and any stdout/reference sidecars used
  by the harness.

The checked-in tests are hosted/vendor compatibility cases, standard-library
sentinels, reducers, and bootstrap-facing compile smokes rather than direct
N3485 clause tests.

Optional sidecars include:

- `x.env`: environment variables for one test, such as additional standard
  include paths
- `x.compile.flags`: extra flags passed to `cppgm++ -c`

The default preprocessor references are intentionally host-agnostic. The test harness checks that checked-in PA34 preprocessor refs do not accidentally
pin local host macro values such as platform-specific integer or floating-point
limits.

### Hosted ABI Name Guidance

PA34 is mostly about accepting hosted headers, but compile-only hosted tests can
still expose symbol-spelling problems through emitted objects and unresolved
references. The `cppgm++ -c` path should continue producing host ABI names that
PA32 and PA33 already made observable.

A recommended implementation style is to keep the PA31 mangler in the
compile-mode path while adding hosted parser, semantic, builtin, and lowering
support. That style works best when hosted standard-library entities, inline
namespaces, ABI-tagged declarations, dependent template names, local entities,
and function template specializations keep the semantic information needed to
name them.

For compile-only tests, the immediate oracle may be successful object emission,
but the object must carry host ABI names that PA36 can link and run without a
separate hosted-only naming scheme.

### Assignment Boundary

PA34 owns hosted compatibility needed before bootstrap, including:

- predefined macro import, `_Pragma`, `__has_*`, `#include_next`, `#warning`,
  ignored unknown pragmas, and hosted hex-float preprocessing forms such as
  `0x1p+4`
- GNU/Clang parser concessions commonly exercised by the selected hosted
  headers, including dependent nested-angle disambiguation, nested qualified
  template-ids used as outer template arguments, builtin-trait identifiers
  referenced as ordinary names, GNU `__decltype`, parenthesized
  throw-expressions emitted by hosted helper macros, and GNU builtin float type
  specifiers such as `__float128` / `_Float128`
- builtin traits, transforms, intrinsics, and builtin families used during
  hosted compile acceptance
- semantic and lowering compatibility for hosted source patterns used by those
  headers and by the bootstrap source base, including post-declarator parameter
  attributes, explicit specializations of primary-template member functions,
  and non-standard hex-float compile acceptance on ordinary floating types

Standard-language bugs discovered here should still be fixed in their true
earlier owner stage when appropriate. PA34 owns the hosted compatibility
pressure, not a second copy of every earlier language rule.

### Out Of Scope

The following are out of scope for PA34:

- host object/link/runtime contracts already owned by PA32 and PA33
- compiling the heaviest hosted standard-library headers, which belongs in PA35
- hosted header-emitted link/runtime behavior, which belongs in PA36
- full build-system emulation beyond the documented query and compatibility
  flags
- recursive hosted-header coverage reporting
- bootstrap or self-host builds

### Design Notes (Non-Normative)

Hosted compatibility is easiest to approach as a sequence of small compatibility
surfaces: preprocessor probes, parser concessions, builtin traits/types, and
then semantic/lowering cases. Keep fixes tied to the source pattern being
exercised. Avoid making broad source-text special cases when an earlier
semantic or template representation can carry the information directly.

That same preference applies to hosted ABI names: keep type structure, template
arguments, ABI tags, and local context available until the ABI naming layer can
consume them. Reconstructing those facts later from pretty-printed strings tends
to create fragile library-version-specific behavior.

### Stage Handoff

The next stage is PA35, which keeps the same hosted source/header environment
but raises the bar to compiling the heaviest hosted standard-library headers
(the perf-gated header-compile workload split out of PA34). PA36 then raises the
contract from "it compiles" to "its emitted code also links and runs."
