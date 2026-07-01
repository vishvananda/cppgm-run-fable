## CPPGM Programming Assignment 33 (`cppgm++ -c`)

### Overview

Write one C++ application called `cppgm++`.

PA33 is the host C++ ABI/runtime interoperability assignment. It builds on the
ordinary host-linkable object contract from PA32 and makes the behavior of the
host-linked program part of the assignment contract.

The main PA33 question is: once host link succeeds, does the resulting program
behave correctly under the ordinary host C++ ABI/runtime?

The tested ABI/runtime surface includes:

- virtual dispatch, vtable ownership, and imported/exported vtables
- RTTI object ownership and `dynamic_cast` / `typeid`
- covariant return adjustment
- richer host exception handling in the exercised
  rethrow/cleanup/noexcept/RTTI subset
- host-compatible unwind and relocation facts where the tests inspect objects

### Prerequisites

Complete PA32 before starting this assignment.

You will want to reuse:

- the full C++ language pipeline through PA32
- the PA32 host-compatible `cppgm++ -c` path
- the PA28 native backend and PA29 object-emission path
- the PA31 exception/runtime lowering concepts

The tests assume a POSIX-like shell environment with `make`, `bash`,
`perl`, and a working host C/C++ toolchain. The harness selects host tools from:

- `CPPGM_HOST_CXX` or `CXX` for the host C++ compiler/link driver
- `CPPGM_HOST_CC` or `CC` for host C helper objects

If those are not set, the harness searches for common compilers such as
`clang++`, `g++`, `c++`, `clang`, `gcc`, and `cc`. Archive and inspection tests
also require `ar`, `nm`, and `readelf`. The checked-in tests assume the normal
x86_64 Linux host C++ ABI.

The basic host-EH object-facts surface is owned by PA31. PA33 keeps the same
host-link path but raises the contract to richer host ABI/runtime interactions.

### Starter Kit

The starter kit provides:

- `dev/cppgm++.cpp`, populated from the `cppgm++` scaffold for the cumulative
  PA10+ compiler driver
- the shared `dev/` sources needed by the scaffold
- `pa33/cppgm++.cpp`, a link to `../dev/cppgm++.cpp`
- `pa33/Makefile`
- `pa33/scripts/`, the host-ABI test harness
- `pa33/tests/general/`, the PA33 tests and checked-in reference files

Student code changes should go in `dev/`, especially `dev/cppgm++.cpp` and the
shared implementation files it calls. Do not edit generated `.my` files. Test
inputs and references are part of the handout unless your instructor asks you
to add or update tests.

There is no separate PA33 reference binary in the starter kit. The checked-in
`.ref.*` files are the oracle.

### Command-Line Contract

PA33 does not introduce new command-line flags. It reuses the PA32 compile-mode
surface:

```sh
cppgm++ -c -o <objfile> <srcfile>
cppgm++ -c --target <target> -o <objfile> <srcfile>
cppgm++ -c -I <dir> -o <objfile> <srcfile>
cppgm++ -c -I<dir> -o <objfile> <srcfile>
cppgm++ -c --target <target> -I <dir> -o <objfile> <srcfile>
cppgm++ -c --target <target> -I<dir> -o <objfile> <srcfile>
```

`<target>` may be `linux` or the corresponding x86_64 Linux host triple form
accepted by your implementation. PA33 only requires compile mode. The normal
PA33 final link is performed outside `cppgm++` by the host C++ compiler driver.

### Output Format

`cppgm++ -c` shall continue to write one host-linker-compatible relocatable
object file to `<objfile>`.

The PA33 tests do not compare object bytes directly. They observe:

- `cppgm++ -c` exit status
- host final-link exit status
- final program exit status
- final program standard output
- optional object-inspection output for ABI, unwind, relocation, RTTI, vtable,
  thunk, and symbol-ownership checks

### Error Handling

If preprocessing, parsing, semantic analysis, lowering, object emission, or
output writing fails, `cppgm++` shall exit with failure.

For negative tests, exact diagnostics are not the grading contract. The harness
compares exit status first. If the reference compile/link path fails, stdout and
stderr are diagnostic side effects rather than required output.

### Testing

Run the PA33 suite with:

```sh
make test
```

To run one test through the shared check target:

```sh
make check TEST=tests/general/200-host-eh-rethrow.t
```

The local tests live in `tests/general/`. They cover host C++ ABI/runtime
behavior, host-linked exception handling, RTTI, vtables, thunks, and object
inspection around those host-runtime surfaces. They are not direct N3485 clause
tests.

For each test anchor `x.t`, companion C++ sources are named:

```text
x.t.1
x.t.2
...
```

Optional sidecars control or check the host flow:

- `x.compile.flags`: extra flags passed to `cppgm++ -c`
- `x.link.flags`: extra flags passed to the host link driver
- `x.lib.*`: host-built C or C++ helper sources
- `x.inspect.cmd`, `x.inspect.expect`, or `x.inspect.plan`: object-inspection
  checks that use the host symbol and object tools

The checked-in PA33 tests cover:

- cleanup, rethrow, noexcept termination, and foreign catch-all behavior beyond
  the basic PA31 host-EH facts surface
- class, base, transitive-base, and virtual-base host exception catches
- host EH interaction with RTTI, `typeid`, lambdas, templates, and control flow
- virtual dispatch, imported/exported vtable ownership, and polymorphic header
  duplication
- RTTI-driven `dynamic_cast` and `typeid`
- covariant return adjustment
- host ABI mangling for dependent/template/lambda/standard-library-adjacent
  names needed by this milestone
- object facts such as unwind sections, relocation classes, weak/undefined
  symbols, and vtable/RTTI ownership when a test includes an inspect sidecar

PA33 does not require general hosted-header support. Your compiler must support
the host ABI behavior for user-defined RTTI and exception cases: emitted RTTI
objects, `dynamic_cast`, `typeid`, vtables, catches, cleanup, and unwind
interoperability. The PA33 tests exercise that behavior without including hosted
headers. For `typeid`, it is enough in this assignment to support the language
operation with a narrow declaration of `std::type_info`; the tests do not depend
on the hosted `<typeinfo>` header or on `std::type_info` member APIs.

Parsing hosted `<exception>` and `<typeinfo>` headers, implementing APIs such
as `std::type_info::name()` or `hash_code()`, compiling hosted exception
classes, and supporting `std::exception_ptr` are later hosted-header/runtime
work.

### Host ABI Symbol Names

PA33 extends the PA32 object contract into host C++ ABI/runtime behavior. The
same ABI naming behavior from PA30 and PA32 is still observable for every C++
symbol that the host linker, unwinder, RTTI system, or virtual-dispatch
machinery can observe.

The important observable result is that these entities have host ABI names that
match the configured toolchain:

- ordinary functions, methods, constructors, destructors, and function
  templates
- vtables, VTTs, RTTI objects, covariant thunks, non-virtual thunks, and
  virtual-base thunks
- exception, cleanup, and helper symbols that must interoperate with host-built
  code
- local classes, lambdas, dependent/template names, ABI tags, and inline
  namespaces when they affect the host name

### Assignment Boundary

PA33 owns practical host-linked C++ ABI/runtime behavior.

To complete PA33, preserve this behavior within the supported subset:

- virtual dispatch and imported/exported vtable ownership
- RTTI-driven `dynamic_cast` / `typeid`
- covariant return adjustment
- ordinary host-linked rethrow and advanced catch behavior
- cleanup and unwind interactions beyond the basic PA31 fact owners
- foreign catch-all interaction in the tested subset

If host link succeeds but the host C++ ABI/runtime behavior is wrong, the issue
belongs in PA33.

### Out Of Scope

The following are out of scope for PA33:

- basic host-EH metadata/object facts, which belong in PA31
- private course-only exception/runtime ABI details that are not visible through
  the host-linked program or object checks
- hosted standard-library header/source compatibility, which belongs in PA34
- hosted header-emitted link/runtime behavior, which belongs in PA36
- bootstrap or self-host builds

### Design Notes (Non-Normative)

PA33 is not just a runtime-output assignment. Runtime behavior is the primary
oracle, but some tests inspect object facts because host C++ ABI correctness is
often decided before the program starts: symbol names, weak ownership, unwind
sections, RTTI/vtable objects, and relocation classes must match the host
toolchain's expectations closely enough for ordinary linking and unwinding.

A recommended implementation style is to continue using the PA30 ABI naming
layer before object emission. Feed semantic facts for the entity into the
mangler, then let the object writer preserve the final raw symbol name. That
keeps ABI spelling decisions close to semantic information and avoids a second
name-construction path in object-format code.

### Stage Handoff

The next stage is PA34, which shifts from host ABI/runtime ownership to hosted
source/header compatibility.
