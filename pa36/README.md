## CPPGM Programming Assignment 36 (`cppgm++ -c`)

### Overview

PA36 is the hosted header-emission and link/runtime compatibility assignment.

By PA34 and PA35, hosted source and heavy hosted headers should preprocess and
compile. PA36 owns the next question: once hosted headers compile, do the
emitted inline, template, and header-generated definitions also link and run
correctly through the host toolchain?

This milestone is narrower than a second general host ABI assignment. It is
specifically about hosted header-emitted code on top of the ordinary host object
and ABI/runtime path established by PA32 and PA33.

### Prerequisites

Complete PA35 before starting this assignment.

You will want to reuse:

- the full earlier language, template, and lowering stack
- the PA34 hosted preprocess/compile compatibility surface
- the PA35 heavy hosted-header compile surface
- the PA32/PA33 host object and ABI/runtime path
- the PA36 demand-driven emitted-symbol model described below

The tests assume a Linux shell environment with `make`, `bash`, `perl`, and a
working host C/C++ toolchain with hosted C++ headers and libraries installed.
You may override the compiler with `CXX=...`.
`CPPGM_HOST_CXX` selects the host compiler/link driver used by the harness and
by host-symbol comparison helpers. If it is not set, it defaults to `CXX`.

PA36 tests also use host object tools:

- `nm` for symbol inspection
- `c++filt` for demangling in host-symbol comparison tests
- `readelf` for selected object/relocation checks
- `ar` and the host C/C++ compilers for helper libraries when a test provides
  `x.lib.*` sidecars

The checked-in tests assume the normal x86_64 Linux host C++ ABI. When you use
a non-default standard library, pass the same choice through
`CPPGM_STDLIB_FLAGS` so the course compiler and host compiler agree.

### Starter Kit

The starter kit provides:

- `dev/cppgm++.cpp`, populated from the `cppgm++` scaffold for the cumulative
  PA10+ compiler driver
- the shared `dev/` sources needed by the scaffold
- `pa36/cppgm++.cpp`, a link to `../dev/cppgm++.cpp`
- `pa36/Makefile`
- `pa36/scripts/`, the hosted link/runtime test harness
- `pa36/tests/link/`, the PA36 tests and checked-in reference files

Student code changes should go in `dev/`, especially `dev/cppgm++.cpp` and the
shared implementation files it calls. Do not edit generated `.my` files. Test
inputs and references are part of the handout unless your instructor asks you
to add or update tests.

There is no separate PA36 reference binary in the starter kit. The checked-in
`.ref.*` files are the oracle.

### Command-Line Contract

PA36 does not introduce new `cppgm++` flags. It reuses the compile-mode surface
already required by PA34:

```sh
cppgm++ -c -o <objfile> <srcfile>
cppgm++ -c --target <target> -o <objfile> <srcfile>
cppgm++ -c -I <dir> -o <objfile> <srcfile>
cppgm++ -c -I<dir> -o <objfile> <srcfile>
cppgm++ -c -isystem <dir> -o <objfile> <srcfile>
cppgm++ -c -D <macro> -U <macro> -include <file> -o <objfile> <srcfile>
```

The normal PA36 final link is performed outside `cppgm++` by the host C++
compiler driver.

### Output Format

`cppgm++ -c` shall continue to write host-linker-compatible relocatable object
files.

The PA36 contract is not a new file format. It is correct symbol ownership,
ABI spelling, and runtime behavior for hosted header-generated code once those
objects are host-linked.

Hosted objects should still be generated from the same LowIR facts exposed by
`cppgm++ --emit-lowir`. If hosted header emission needs symbol ownership,
object symbol spellings, TLS wrapper facts, or runtime hooks, those facts
belong in LowIR metadata, declarations, definitions, or object aliases rather
than in a hosted-only side channel.

The PA36 tests observe:

- `cppgm++ -c` exit status
- host final-link exit status
- final program exit status
- final program standard output
- optional object-inspection output for symbol ownership, unresolved-symbol
  spelling, relocation, and ABI checks

### Error Handling

If preprocessing, parsing, semantic analysis, lowering, object emission, host
linking, or output writing fails, the relevant tool invocation shall report
failure. For `cppgm++`, that means exiting with failure.

For negative tests, exact diagnostics are not the grading contract. The harness
compares exit status first. If the reference compile/link path fails, stdout and
stderr are diagnostic side effects rather than required output.

### Hosted Symbol Emission Surface

Hosted headers expose many inline functions, function templates, constants,
helpers, and implementation-detail declarations. PA36 does not require
`cppgm++ -c` to emit every hosted entity that was parsed, referenced during
semantic analysis, or made visible by an include.

The emitted object should be demand-driven:

- emit the definitions needed by the current translation unit's generated code
  and by the required-definition closure of those definitions
- keep ordinary declarations, overload candidates, template patterns, and
  unused inline/header helpers available for semantic analysis without turning
  them into defined object symbols
- allow unresolved references for externally owned hosted library symbols, using
  the host ABI spelling expected by the configured toolchain

This keeps hosted object files small and avoids exporting implementation-detail
symbols just because a broad standard-library header was included. For example,
including `<functional>`, using `std::forward`, using placement `new`, or
instantiating an `unordered_set` should not by itself cause unrelated libc++ or
libstdc++ helper definitions to appear as defined symbols in the output object.

### Hosted Mangling Rules

PA36 uses the ordinary host C++ ABI spelling. Generic substitution mechanics
should be tested with source-level spellings, and hosted standard-library
ABI checks should be derived from the configured host compiler rather than from
hard-coded library-private names.

The hosted emission policy decides which entities are defined or left
unresolved. Those decisions still need to preserve ordinary host ABI spelling
for every emitted or referenced hosted symbol.

For Itanium-style mangling on GNU/libstdc++ and Clang/libc++ style hosts:

- direct standard-library substitutions such as `St`, `So`, `Si`, `Sd`, `Ss`,
  and `Sa` must use the host ABI spellings
- numbered substitutions must continue across the whole mangled entity, not
  restart between the function-name template-argument list and the bare function
  type
- direct standard substitutions do not themselves become numbered substitution
  entries
- hosted weak/header-emitted definitions must still use the same symbol names
  that the host library expects for the corresponding inline/template bodies

In practice, PA36 symbol tests should prefer source spellings, such as
`std::string` versus `std::basic_string<char, std::char_traits<char>,
std::allocator<char>>`, and compare the unresolved symbol surface against the
configured host compiler where the exact raw spelling is library-specific.

PA36 symbol tests should avoid hard-coding implementation namespaces such as
`std::__1` or `std::__cxx11` unless the test is explicitly guarded for that host
library.

### Testing

Run the PA36 suite with:

```sh
make test
```

To run one test through the shared check target:

```sh
make check TEST=tests/link/600-hosted-std-function-call-link-smoke.t
```

The local tests live in `tests/link/`. The directory name reflects the oracle:
hosted compile plus host final link/run, with optional object inspection for
symbol ownership and unresolved-symbol checks.

For each test anchor `x.t`, companion C++ sources are named:

```text
x.t.1
x.t.2
...
```

Optional sidecars include:

- `x.compile.flags`: extra flags passed to `cppgm++ -c`
- `x.link.flags`: extra flags passed to the host link driver
- `x.env`: environment variables for one test
- `x.lib.*`: host-built C or C++ helper sources
- `x.inspect.cmd` or `x.inspect.expect`: object/symbol checks

Some PA36 tests inspect intermediate object files with `nm`-style expectations.
These checks verify positive ownership — a needed inline/template definition is
present and correctly ABI-spelled. (Earlier negative-ownership / elision checks,
which asserted that unused hosted helpers stay absent from the defined-symbol
table, have been dropped: which internal symbols an object omits is an
implementation detail, not a conformance requirement.)

The checked-in tests are hosted link/runtime smokes, ABI spelling checks, and
object-inspection checks rather than direct N3485 clause tests.

### Assignment Boundary

To complete PA36, implement hosted link/runtime behavior for:

- emitted inline/template/header definitions from hosted headers needed by the
  current object
- hosted standard-library code that compiles in PA34 or PA35 but still has to
  link and run through the plain host toolchain
- hosted link smokes where the main question is emitted symbol ownership, ABI
  spelling, or runtime behavior of hosted header-generated code

If hosted header code compiles but the emitted objects do not link or run
correctly, the issue belongs in PA36.

### Out Of Scope

The following are out of scope for PA36:

- earlier hosted preprocess/compile compatibility already owned by PA34 and
  PA35
- general host object or host ABI ownership outside the hosted-header-triggered
  surface already owned by PA32/PA33
- build-system wrapper emulation
- recursive hosted-header coverage reporting
- bootstrap or self-host builds

### Design Notes (Non-Normative)

Treat header-emitted code as ordinary code with an ABI-sensitive ownership
policy. The implementation should preserve enough semantic information to know
which inline/template definitions are required, which declarations remain
external, and which unused hosted helpers should stay un-emitted.

A recommended integration style is to use the PA30 ABI naming layer for hosted
symbols in the same way PA32 and PA33 use it for ordinary host objects. Semantic
analysis can produce the facts for the entity being emitted or referenced, then
the mangler can produce the final raw symbol name before object emission. When a
hosted symbol case is missing information, prefer threading that semantic fact
forward instead of building already-mangled or partly-mangled strings in later
object/link stages.

### Stage Handoff

The hosted compatibility work from PA34, PA35, and PA36 prepares the later
optimizer and self-host stages to compile larger source bases with the course
compiler.
