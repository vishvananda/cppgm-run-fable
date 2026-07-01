# PA13 LowIR Specification

This document defines the LowIR format used as input to PA13.

LowIR is intended to be the long-term backend boundary for the later lowering-heavy
assignments. PA13 only implements a procedural subset of the full IR, but the overall
structure is defined early so later assignments extend the same IR rather than replacing it.

LowIR is deliberately:

- lower-level than the PA12 semantic dump
- higher-level than CY86
- structured enough to support later native backends and optimization passes
- simple enough that the PA13 subset can be translated mechanically into CY86

## Core Design

LowIR is:

- block-based
- text-based and deterministic
- explicit about globals, functions, stack slots, temporaries, and control flow
- non-SSA by requirement, but compatible with later SSA conversion if desired

LowIR text is the serialized form of the compiler-owned backend program model.
Implementations may use a typed in-memory representation, and the starter
support includes an optional `dev/src/lowir_model.h` scaffold for that shape,
but the text remains the durable boundary. A backend-visible fact that cannot
be serialized to LowIR and parsed back is not part of the LowIR contract.

The following naming conventions are used:

- globals: `@name`
- functions: `@name`
- blocks: `^label`
- stack slots: `$name`
- temporaries: `%name`

These are LowIR symbol names, not necessarily raw source-level identifiers. Later C++
lowering assignments may encode qualified or overloaded source names into deterministic
LowIR symbol spellings as long as they remain valid `@name` tokens.

## Types

The base LowIR type family begins with the scalar and pointer types:

- `void`
- `i1`
- `i8`
- `u8`
- `i16`
- `u16`
- `i32`
- `u32`
- `i64`
- `f32`
- `f64`
- `f80`
- `ptr`

PA13 also accepts a restricted object-aware direct call-boundary type:

- `obj<bytesxalign>`

where `bytes` is the storage size in bytes and `align` is the required alignment in
bytes.

This form is part of the semantic call boundary. In the current PA13 subset it is valid in
direct parameter positions, function return positions, explicit indirect-call signatures,
and addressable object slot declarations when later lowering stages need concrete local
storage. It is still not a general-purpose structured global-data item type.

## Program Structure

A LowIR program is a sequence of top-level declarations and definitions.
Canonical LowIR reference output and canonical dumps use this top-level
presentation order:

1. `declare global`
2. `declare function`
3. `global`
4. `function`
5. `alias object`

For canonical text, no later phase is followed by an earlier phase. For example,
all global definitions appear before all function definitions, and all
declarations appear before definitions.

Within canonical text, each phase's output order is defined for the same input
and command line. Source-owned top-level entries are ordered by command-line
translation-unit order and then source/declaration order within each
translation unit. Generated top-level entries, including helper globals, helper
functions, init/fini functions, thunks, and runtime-support entries, follow the
generated-definition rules below. If no more specific rule below applies, emit
generated entries in first semantic-demand order using command-line
translation-unit order and source/declaration order as the traversal order.
For reference presentation, generated support entries that are collected from
symbol sets rather than one semantic traversal point may use internal LowIR
symbol-name order as a tie-breaker. That fallback is a presentation detail, not
a semantic dependency: source-to-LowIR assignment comparison may canonicalize
top-level entries before comparison, and implementations should not make host
ABI mangling, `object=...` metadata, or backend symbol spelling the owner of
semantic ordering facts.

For C++ source-to-LowIR reference output, the following generated-definition
ordering is the canonical presentation even when another top-level order would
have the same execution behavior:

- Source-owned non-mergeable function definitions appear in command-line
  translation-unit order and then source/declaration order.
- Demand-emitted definitions, such as inline, template-instantiated, weak, or
  synthesized helper definitions, appear after the source-owned definitions that
  make them required. If one emitted helper requires another helper, the newly
  required helper appears later in the same `function` phase.
- For one class, generated copy forms precede generated move forms within the
  same special-member family: copy constructor before move constructor, and copy
  assignment before move assignment.
- Constructor ABI entry points for the same constructor appear as base entry
  before complete entry. Destructor ABI entry points for the same destructor
  appear as base entry, deleting entry, then complete entry.
- Vtable-related global definitions appear in the `global` phase. For one
  polymorphic class, the primary vtable appears before secondary/view vtables,
  secondary/view vtables follow complete-object subobject layout order, and
  any VTT appears after the vtables it references.
- Vtable slots follow virtual-slot order. Inherited primary-base slots precede
  slots newly introduced by the class, newly introduced slots follow source
  declaration order, and an override keeps the slot of the function it
  overrides. A virtual destructor occupies two adjacent vtable slots: complete
  destructor entry first, then deleting destructor entry. The base destructor
  entry is not a vtable slot. This vtable slot order is intentionally different
  from the function-definition order above.
- Generated construction helpers for base and member subobjects follow C++
  lifetime order. Generated destruction helpers follow the corresponding reverse
  lifetime order.
- Global initialization helpers appear before global finalization helpers.
  Initialization actions follow declaration order; finalization actions follow
  reverse declaration order.

Top-level definitions and declarations may refer to symbols that are emitted
later in the same LowIR program. The canonical top-level order is a stable
presentation convention, not a dependency sort. Later source-to-LowIR
assignment harnesses may canonicalize top-level entries before comparison, but
they still preserve and check order-sensitive LowIR regions: instruction order
inside blocks, item order inside structured globals, vtable slot order, and
action order inside generated initialization, finalization, constructor,
destructor, and cleanup bodies.

The declaration/definition split is explicit:

```text
declare global @name
declare global @name : <type>
declare function @name(%a : i64, %b : ptr) -> i64
declare function @printf(%fmt : ptr [pass=decay]) -> i32 [arity=variadic]
declare function @legacy() -> i64 [arity=prototype_relaxed]
```

Top-level declarations and definitions may also carry explicit symbol metadata:

```text
declare global @shared_state : ptr [binding=weak]
declare global @__external_rtti__int : ptr [binding=strong, object=_ZTIi]
declare global @ro_table : ptr [storage=readonly]
declare global @tls_state : i64 [storage=thread_local]
declare function @user_entry() -> i64 [role=entry]
declare function @puts(%fmt : ptr [pass=decay]) -> i32 [arity=variadic, linkage=c]
global @exc_top : ptr [role=eh_top, storage=readonly] = zero
global @tls_counter : i64 [storage=thread_local] = 7
function @main() -> i64 [role=entry, binding=strong, keep_alias=yes] {
  ...
}
function @helper() -> i64 [binding=strong, object=_ZL6helperv, prefer_local=yes] {
  ...
}
function @Tag__Tag(%this : ptr) -> void [binding=weak, trivial_lifecycle=yes] {
  ...
}
function @boot() -> void [role=init, binding=strong] {
  ...
}
```

The currently defined top-level metadata keys are `role`, `linkage`, `binding`, `object`,
`tls_for` (functions only), `keep_alias`, `prefer_local`, `trivial_lifecycle`
(functions only), and `storage` (globals only).

The currently defined global `storage` values are:

- `readonly`
- `thread_local`

`storage=thread_local` is semantic TLS storage intent. Backend-specific descriptor
sections and relocation forms are lower-layer object-format details. When a thread-local
object needs a named ABI wrapper function surface, that surface appears as an ordinary
LowIR function declaration or definition with `tls_for=<global>` naming the thread-local
global it wraps, for example
`declare function @x__tls_wrapper() -> ptr [object=_ZTW..., tls_for=@x]`.

The currently defined role values are:

- function roles:
  - `entry`
  - `init`
  - `fini`
  - `eh_unhandled`
  - `eh_allocate_exception`
  - `eh_begin_catch`
  - `eh_call_unexpected`
  - `eh_current_exception_type`
  - `eh_end_catch`
  - `eh_rethrow`
  - `eh_throw`
  - `eh_personality`
  - `eh_resume`
- global roles:
  - `eh_top`
  - `eh_value`
  - `eh_type`

Only one symbol may own each singleton role inside a LowIR program. A role must also appear
on the correct top-level kind: function roles on functions and global roles on globals.

The currently defined linkage values are:

- `c`
- `cpp`

`cpp` is the ordinary default and is normally omitted. `c` makes an otherwise ordinary symbol
explicitly C-linked in the printed LowIR. The current emit-LowIR path uses `linkage=c` where
the semantic frontend already knows the symbol has C linkage, such as `extern "C"` imports.

The currently defined binding values are:

- `internal`
- `strong`
- `weak`

`binding` makes the intended top-level symbol binding explicit in printed LowIR. This is the
language/backend-neutral ownership boundary that later object emission can interpret as local,
ordinary external, or weak/coalescable.

The `object` metadata key carries an explicit backend/object symbol spelling for the top-level
symbol. This is the textual carrier for cases where later object emission must use a concrete
host/object name such as `puts`, `_ZTIi`, or a local mangled helper name.

The `keep_alias` metadata key is a `yes`/`no` flag. `keep_alias=yes` requests that later object
emission keep the LowIR-internal symbol spelling available as an alias even when `object=...`
spells a different exported/backend name.

The `prefer_local` metadata key is a `yes`/`no` flag. `prefer_local=yes` records that later
object emission should prefer a local/private binding when that is legal, even if an explicit
`object=...` spelling is also present.

The `trivial_lifecycle` metadata key is a `yes`/`no` flag for top-level
function declarations and definitions. `trivial_lifecycle=yes` records that the
function is a semantically trivial C++ lifecycle helper, such as a trivial
constructor or destructor wrapper. Object lowering may use this fact to remove
otherwise unreferenced weak lifecycle wrappers after the frontend has serialized
the LowIR boundary.

`alias object <object-symbol> = @target` records an additional object-file symbol spelling
that must resolve to the same emitted top-level LowIR function or global as `@target`.
Multiple alias lines may target the same LowIR symbol, but each alias object symbol may be
defined only once:

```text
function @Box__Box(%this : ptr) -> void [binding=weak, object=_ZN3BoxC1Ev] {
  ...
}
alias object _ZN3BoxC2Ev = @Box__Box
```

Use an object alias only when the extra symbol is another name for the same emitted code or
data. If the extra ABI surface is separate generated code, such as a thread-local access
wrapper, represent it as a normal LowIR function declaration or definition with its own
`object=...` spelling.

Canonical `cppgm++ --emit-lowir` output is expected to carry all backend-relevant symbol/export
and runtime-support information needed by later object preparation. Later object-path steps such
as debug stripping or LowIR optimization may transform that canonical program, but they should
not depend on hidden semantic side data that is absent from textual LowIR. An in-process
`cppgm++ -c` implementation may skip writing a temporary `.lowir` file for speed, but it should
operate on facts that could have been obtained by serializing the LowIR program and parsing it
again.

Functions may also carry explicit call-boundary metadata after the return type:

```text
declare function @printf(%fmt : ptr [pass=decay]) -> i32 [arity=variadic, effects=readonly]
function @sum(%count : i64) -> i64 [arity=variadic, effects=readwrite] {
  ...
}
declare function @legacy() -> i64 [arity=prototype_relaxed]
declare function @trap() -> void [effects=readnone, unwind=no, return=noreturn]
```

The currently defined function metadata keys are:

- `arity`
- `effects`
- `unwind`
- `return`

The currently defined arity values are:

- `fixed`
- `variadic`
- `prototype_relaxed`

`fixed` is the default and is normally omitted. `variadic` means the function accepts at least
its declared parameter prefix plus any later variadic arguments. `prototype_relaxed` means the
boundary does not promise strict fixed-arity checking at the call site; the current structural
validator treats it as accepting at least its declared parameter prefix, like a more weakly
specified variadic boundary. The current C++ emit path still only produces `fixed` or
`variadic`; the `prototype_relaxed` spelling exists so LowIR can carry that boundary mode
explicitly once an upstream frontend models it truthfully.

The currently defined call-effect values are:

- `readnone`
- `readonly`
- `readwrite`

`readwrite` is the ordinary conservative default when omitted. `readnone` means the boundary
does not read or write memory. `readonly` means the boundary may read memory but does not
write it.

The currently defined unwind values are:

- `may`
- `no`

`may` is the ordinary default when omitted. `no` means the boundary does not unwind.

This is an IR-level fact, not a promise about how every frontend always infers
it. In particular, the current `cppgm++ --emit-lowir` path only infers
`unwind=no` from cheap explicit source forms such as bare `noexcept`,
`throw()`, and trivial boolean `noexcept(...)` expressions. Arbitrary explicit
`noexcept(expr)` may still omit `unwind=no` until that semantic path has a
cheap non-text-based implementation.

The currently defined return values are:

- `returns`
- `noreturn`

`returns` is the ordinary default when omitted. `noreturn` means the boundary never returns
normally.

Call signatures only accept call-boundary metadata such as `arity=...`, `effects=...`,
`unwind=...`, and `return=...`. Top-level symbol metadata such as `role=...`, `linkage=...`,
`binding=...`, `object=...`, `keep_alias=...`, `prefer_local=...`, and
`trivial_lifecycle=...` is not valid on `as (...) -> ...` call signatures.

Later backend lowering may also introduce internal compiler builtin helper symbols for
operations that are still first-class in LowIR but are not emitted directly on the
current target. Those helpers should stay in the `cppgm_builtin_*` namespace inside the
compiler pipeline; later object emission may remap them to the host runtime's concrete
symbol spellings.

Definitions keep the existing `global` and `function` forms.

Example:

```text
declare function @helper(%x : i64) -> void
declare global @ext_state : ptr

global @g : i64 = zero

function @user_entry() -> i64 [role=entry] {
  slot $x : i64

  block ^entry:
    %0 = const i64 0
    store i64 %0, $x
    %1 = load i64 $x
    return i64 %1
}
```

## Globals

Declaration-only globals use:

```text
declare global @name
declare global @name : <type>
```

The typed form should be used when the storage has a stable scalar/pointer-like
LowIR type. The untyped form is used for opaque imported storage whose concrete
layout is not part of the current IR unit.

The PA13 subset requires:

```text
global @name : <type> = zero
global @name : <type> = <scalar-literal>
global @name : ptr = addr @other
global @name : <type> [storage=readonly] = <scalar-literal>
global @name : <type> [storage=thread_local] = <scalar-literal>
```

Address initializers may name either another global or a function.

The PA13 milestone must also support the structured global-data form below. Later milestones
may use it more heavily without changing the surrounding global definition structure.

### Structured Global Data Initializers

PA13 must support a structured global-data form for read-only tables, string-like byte data,
aggregate constants, and vtable storage:

```text
global @name = {
  <data-item>
  <data-item>
  ...
}

global @name [storage=readonly] = {
  <data-item>
  <data-item>
  ...
}

global @name [storage=thread_local] = {
  <data-item>
  <data-item>
  ...
}
```

Where each `<data-item>` is one of:

```text
<type> <scalar-literal>
ptr addr @symbol
zero <bytes>
```

Examples:

```text
global @vtable [storage=readonly] = {
  ptr addr @YD__f
  ptr addr @YD__g
}

global @msg = {
  i8 72
  i8 105
  i8 0
}

global @obj = {
  i64 1
  zero 8
  ptr addr @helper
}

global @coeffs = {
  f32 1.5f
  f64 2.25
}
```

For compatibility, the parser still accepts the bare `readonly` and `thread_local`
keywords on global declarations and definitions. Printed LowIR normalizes both forms to
`[storage=...]`.

This form is intended to describe final data layout directly. It does not imply any
source-language construction semantics by itself. When translated through PA13, multi-byte
items use their natural CY86 data width and alignment, while `zero <bytes>` contributes raw
padding/data bytes exactly as written.

## Functions

Declaration-only functions use:

```text
declare function @name(%a : i64, %b : ptr) -> i64
```

A declaration names the imported/direct-call surface of the current IR unit
without providing a body.

A function has:

- a name
- a parameter list
- a return type
- zero or more stack slots
- one or more basic blocks

Function header syntax:

```text
function @name(%a : i64, %b : ptr) -> i64 {
  ...
}
```

Optional top-level function metadata appears after the return type:

```text
function @user_entry() -> i64 [role=entry] {
  ...
}
```

Multiple function metadata items may appear in one bracket or separate brackets:

```text
function @sum(%count : i64) -> i64 [arity=variadic] {
  ...
}

function @user_entry() -> i64 [arity=fixed, role=entry] {
  ...
}
```

Function definitions may also carry an optional trailing debug-location suffix
after the function metadata:

```text
function @user_entry() -> i64 [role=entry] !dbg(main.cpp, 1, 1) {
  ...
}
```

Parameters may also carry explicit passing metadata after the parameter type:

```text
function @helper(%ret : ptr [pass=indirect_result],
                 %obj : ptr [pass=by_address],
                 %ref : ptr [pass=reference],
                 %arr : ptr [pass=decay],
                 %buf : ptr [capture=nocapture, access=read, alias=noalias],
                 %x : i64) -> void {
  ...
}
```

The currently defined parameter metadata keys are `pass`, `capture`, `access`,
and `alias`.

The currently defined pass values are:

- `direct`
- `indirect_result`
- `by_address`
- `reference`
- `decay`

The currently defined capture values are:

- `nocapture`
- `maycapture`

The currently defined access values are:

- `none`
- `read`
- `write`
- `readwrite`

The currently defined alias values are:

- `noalias`

The intended meaning is semantic, not host-ABI-specific:

- `indirect_result`: this pointer names caller-owned result storage
- `by_address`: this parameter is an indirect object/value boundary
- `reference`: this parameter is a reference boundary
- `decay`: this parameter came from array/function decay
- `direct`: ordinary direct-value passing
- `nocapture`: the callee does not retain the incoming pointer value beyond the call
- `maycapture`: the callee may retain or otherwise capture the incoming pointer value
- `none`: the callee does not dereference the incoming pointer
- `read`: the callee only reads through the incoming pointer
- `write`: the callee only writes through the incoming pointer
- `readwrite`: the callee may both read and write through the incoming pointer
- `noalias`: this incoming pointer is disjoint from every other pointer
  parameter on the same boundary that also carries `alias=noalias`

For the current LowIR subset, non-`direct` pass modes require parameter type `ptr`.
Capture, access, and alias metadata also currently require parameter type `ptr`.
`indirect_result` must appear on the first parameter and requires function return type `void`.

Stack slot syntax:

```text
slot $name : <type>
```

Slots may use ordinary scalar types or addressable object storage types such as
`obj<16x8>` when later lowering stages need concrete local object backing storage.

Blocks are written in order:

```text
block ^label:
  ...
```

Each block must end in exactly one terminator instruction.

### Debug Locations

LowIR function definitions and instructions may carry optional trailing
source-location metadata:

```text
function @main() -> i32 !dbg(main.cpp, 1, 1) {
  block ^entry:
%t = const i64 42 !dbg(main.cpp, 3, 7)
return i64 %t !dbg(main.cpp, 4, 3)
}
```

The `!dbg(...)` suffix is the current PA13-facing carrier for emitted
debug-info locations. On function definitions it records the function-level
source location. On instructions it records instruction-level locations.

The current fields are:

- source file
- 1-based source line
- 1-based source column

In the current textual format, the source-file field must be a single unquoted
LowIR token. Relative Unix-style source paths such as `main.cpp`,
`src/main.cpp`, or `tests/input.t` are valid. The parser rejects zero or
negative line/column values.

This metadata is semantically optional. Backends that do not yet emit
object-file debug information may ignore it, but PA13 parsing, dumping, and
`lowiropt` transport are expected to preserve it when present.

## Structural Well-Formedness

A LowIR unit is structurally well formed only if all of the following hold:

- every top-level symbol name appears at most once across declarations and definitions
- every object alias spelling appears at most once
- every object alias target names a top-level declaration or definition
- every `tls_for` target names a top-level thread-local global, and at most one
  wrapper names each thread-local global
- each function parameter name is unique within that function header
- each function slot name is unique within that function
- each function block label is unique within that function
- every function contains at least one block
- every block ends in exactly one terminator instruction
- no instruction appears after a terminator within the same block
- every referenced block target exists in the same function

The PA13 parser and validator are required to reject structurally malformed LowIR. Examples
of malformed programs include:

- a second `block ^entry:` inside the same function
- a second `slot $x : i64` inside the same function
- a `return` followed by another instruction in the same block
- a `jump ^missing` where `^missing` is never defined
- both `declare function @main(...)` and `function @main(...)` in the same LowIR unit

## Object ABI Conventions

LowIR stays semantic at the call boundary. Later object-aware milestones use explicit
call-boundary metadata plus restricted direct object boundary types where necessary,
rather than pretending that host register chunking is the stable IR contract.

The intended convention is:

- complete object values passed directly may use a semantic direct parameter type such as
  `obj<4x4>` or `obj<16x8>`
- complete object values passed indirectly use `ptr [pass=by_address]`
- complete object return-by-value may use a semantic direct return type such as
  `-> obj<8x4>` when the frontend wants that boundary explicitly represented in LowIR
- complete object return-by-value may also use an explicit leading destination pointer
  `ptr [pass=indirect_result]` when the source boundary is itself indirect
- source-level references use `ptr [pass=reference]`
- decayed array/function parameters use `ptr [pass=decay]`
- variadic functions use explicit `arity=variadic` metadata on the function declaration or
  definition
- complete object locals may be represented by addressable storage rather than scalar
  temporaries, for example `slot $tmp : obj<16x8>`

So later milestones may use function headers such as:

```text
function @make_small_pair(%a : i64, %b : i64) -> obj<8x4> {
  ...
}

function @make_pair(%ret : ptr [pass=indirect_result], %a : i64, %b : i64) -> void {
  ...
}

function @consume_pair(%p : ptr [pass=by_address]) -> void {
  ...
}

function @sum_pair(%p : obj<8x4>) -> i64 {
  ...
}

function @consume_ref(%p : ptr [pass=reference]) -> void {
  ...
}
```

These annotations describe the semantic call boundary, not the final host register
assignment. Host-specific coercions such as pair-in-register splitting remain backend work,
not stable LowIR syntax.

PA13 must accept and translate these object-lowered forms even though PA14+ are the first C++
lowering milestones that are expected to generate them routinely.

## Reserved Runtime Hooks

Later lowering milestones may introduce synthetic helper functions and globals that
participate in program startup, shutdown, or exception/runtime support.

The preferred convention is role-driven:

- one `function` definition with `[role=entry]`
- zero or one `function` definition with `[role=init]`
- zero or one `function` definition with `[role=fini]`

If present as definitions, backend adapters such as PA13 `lowir2cy86` should run the
`init` hook before the `entry` function and the `fini` hook after it.

Later hosted/EH lowering may also introduce reserved runtime-support roles:

- globals:
  - `eh_top`
  - `eh_value`
  - `eh_type`
- functions:
  - `eh_unhandled`
  - `eh_allocate_exception`
  - `eh_begin_catch`
  - `eh_call_unexpected`
  - `eh_current_exception_type`
  - `eh_end_catch`
  - `eh_rethrow`
  - `eh_throw`
  - `eh_personality`
  - `eh_resume`

For compatibility with earlier handwritten LowIR and pre-role test cases, the legacy
spellings `@main`, `@__cppgm_init`, and `@__cppgm_fini` are still accepted when no explicit
role metadata is present. New LowIR should prefer explicit roles over those special
spellings.

## Operands

The PA13 subset uses:

- temporaries: `%0`, `%tmp`
- stack slots: `$x`
- globals/functions: `@g`, `@main`
- block labels for terminators: `^entry`
- scalar literals (`integer` or `floating`)

## Required PA13 Instructions

### Constants and Copies

```text
%t = const <type> <literal>
%t = copy <type> <value>
```

`<literal>` may be either an integer literal or a floating literal. Floating literals are
valid with `f32`, `f64`, and `f80` types. `f80` literals use the usual `L` suffix, for
example `1.25L`.

When adapting this IR to a runnable backend that lacks direct 80-bit registers, a practical
representation is:

- `f80` object storage remains 16 bytes in memory
- the first 10 bytes carry the executable float80 payload
- the trailing 6 bytes are treated as zero padding

That keeps `index f80`, structured global layout, and later native/object lowering aligned
with the same LowIR storage model while still allowing PA13-style execution through a
different internal calling convention if needed.

### Memory and Addressing

```text
%t = addr @global
%t = addr @function
%t = addr $slot
%t = load <type> <storage>
store <type> <value>, <storage>
%t = atomic_load <type> <ptr-value>, <order>
atomic_store <type> <value>, <ptr-value>, <order>
%t = atomic_exchange <type> <ptr-value>, <value>, <order>
%t = atomic_compare_exchange <type> <ptr-value>, <expected-ptr>, <desired>, <success-order>, <failure-order>
%t = index <type> [projection=<kind>] <base>, <offset>
```

In the PA13 subset, `index` computes a pointer by advancing a base pointer by `offset`
elements of `<type>`.

`[projection=<kind>]` is optional metadata. When present, it makes the semantic address
intent explicit instead of leaving later passes to reconstruct it from frontend-only
lowering shape. The currently supported projection kinds are:

- `array_element`
- `field`
- `base_subobject`
- `reference_field`

When omitted, `index` still means plain pointer arithmetic with no stronger semantic
projection claim.

Atomic memory-order operands use the GNU/Clang order encoding:

- `0` relaxed
- `1` consume
- `2` acquire
- `3` release
- `4` acq_rel
- `5` seq_cst

### Bulk Object Memory Operations

PA13 must support explicit bulk-memory instructions instead of requiring every object copy or
zero-initialization path to be spelled as a hand-written sequence of scalar `load`/`store`
operations:

```text
copyobj <bytes>x<align> <src>, <dst>
zeroinit <bytes>x<align> <dst>
```

Where:

- `<bytes>x<align>` is a positive byte-count/alignment span
- `<align>` must be a positive power-of-two byte alignment
- `<src>` is either:
  - a pointer-valued operand, or
  - a direct object-typed value `obj<bytesxalign>` whose byte size and alignment
    match `<bytes>x<align>`
- `<dst>` is a pointer-valued operand

`copyobj` copies exactly `<bytes>` bytes from `<src>` to `<dst>`. When `<src>` is an
object-typed value, the operation copies from that semantic object value rather than
requiring the IR to spell the host ABI chunking explicitly.

`zeroinit` writes zero to exactly `<bytes>` bytes starting at `<dst>`.

These instructions are intended for lowered object storage, trivial copies, and simple static
zero-initialization paths. They do not imply source-language copy semantics by themselves;
later milestones still decide when a copy constructor, destructor, or other semantic action
is required.

### Unary and Binary Operations

```text
%t = unary <op> <type> <value>
%t = binary <op> <type> <lhs>, <rhs>
%t = cmp <pred> <type> <lhs>, <rhs>
%t = convert <op> <dst-type> <src-type> <value>
%t = atomic_add_fetch <type> <ptr-value>, <delta>, <order>
atomic_thread_fence <order>
atomic_signal_fence <order>
```

Required PA13 unary operators:

- `neg`
- `not`
- `bitnot`
- `decay` for `ptr`
- `bswap` for `i16`, `i32`, and `i64`

Required PA13 binary operators:

- `add`
- `sub`
- `mul`
- `div`
- `mod`
- `udiv`
- `umod`
- `and`
- `or`
- `xor`
- `shl`
- `shr`
- `ushr`

For floating types, the required binary subset is:

- `add`
- `sub`
- `mul`
- `div`

Required PA13 comparison predicates:

- `eq`
- `ne`
- `lt`
- `le`
- `gt`
- `ge`
- `ult`
- `ule`
- `ugt`
- `uge`

For `cmp`, `<type>` is the comparison operand type, not the result type. The
destination temporary is a canonical integer truth value and is materialized as
`i64` `0` or `1`. This is true for floating comparisons such as
`%ok = cmp eq f80 %a, %b`, pointer comparisons, and narrow integer comparisons;
do not allocate or propagate the destination temporary as `f80`, `ptr`, `i16`,
or the other operand type.

For integer operations where signedness changes the result, the plain forms remain the signed
operations:

- `div`, `mod`, `shr`, `lt`, `le`, `gt`, `ge`

and the `u...` forms are the corresponding unsigned operations:

- `udiv`, `umod`, `ushr`, `ult`, `ule`, `ugt`, `uge`

`atomic_add_fetch` atomically adds `<delta>` into the pointed storage and returns the
updated value.

`atomic_exchange` atomically replaces the pointed storage with `<value>` and returns the
previous value.

`atomic_compare_exchange` atomically compares the pointed storage against `*<expected-ptr>`.
If they compare equal, it stores `<desired>` and returns `1`. Otherwise it writes the current
memory value back through `<expected-ptr>` and returns `0`.

`unary bswap` performs byte reversal within the operand width. For example:

- `i16 0x1234` becomes `0x3412`
- `i32 0x11223344` becomes `0x44332211`
- `i64 0x0102030405060708` becomes `0x0807060504030201`

For runnable adapters such as PA13 `lowir2cy86`, a good implementation strategy is:

- use a direct machine/runtime byte-swap instruction where the execution substrate has one
- otherwise lower `i16` through a small shift/or sequence
- keep the operation as a first-class LowIR unary op rather than expanding it during C++
  lowering, so later native/object backends can choose the best lowering directly

`unary decay ptr` is a semantic identity on pointers. It exists to mark the point where an
array or function entity decays into a pointer view, instead of forcing later passes to
reconstruct that fact from surrounding address arithmetic alone.

In the PA13 `lowir2cy86` scaffold, these operations may use a principled single-threaded
interpretation:

- `atomic_load` / `atomic_store` lower like ordinary memory accesses
- `atomic_exchange` lowers like a load / store pair that returns the previous value
- `atomic_compare_exchange` lowers like a compare / conditional store sequence that also
  updates the expected-value storage on failure
- `atomic_add_fetch` lowers like a load/add/store sequence that returns the updated value
- fences are accepted and may lower to no code because CY86 has no thread model

This compare-exchange primitive is also the intended stable LowIR building block for more
complex atomic read-modify-write operations. Later lowering stages may implement operations
such as atomic fetch-and, fetch-or, and fetch-xor by expanding them into compare-exchange
loops without changing the LowIR surface again.

Later native/object milestones must preserve the same LowIR source contract while providing
real machine-level atomic behavior.

For floating types, the same predicate spellings apply over `f32` / `f64` / `f80`
operands.

Required PA13 scalar conversion operators:

- `sext`
- `zext`
- `trunc`
- `sitofp`
- `uitofp`
- `fptosi`
- `fptoui`
- `fpext`
- `fptrunc`

These conversion operators make the signedness and direction explicit in the IR:

- `sext` / `zext`
  widen an integer value from a narrower explicit source width into a wider
  destination width
- `trunc`
  narrows an integer value to a smaller destination width
- `sitofp` / `uitofp`
  convert integer values to floating values
- `fptosi` / `fptoui`
  convert floating values to integer values
- `fpext`
  widens a floating value to a larger floating type
- `fptrunc`
  narrows a floating value to a smaller floating type

Examples:

```text
%w = convert zext i64 u8 %byte
%x = convert sext i64 i32 %word
%y = convert trunc i16 i64 %wide
%a = convert sitofp f64 i32 %x
%b = convert uitofp f80 i64 %u
%c = convert fptosi i32 f64 %f
%d = convert fpext f80 f32 %z
%e = convert fptrunc f32 f80 %ld
```

The plain integer types in LowIR do not encode signedness by themselves, so the signedness
for integer/float conversions lives in the operator spelling rather than the type text.

### Stack allocation

```text
%p = stack_alloc <size>
```

`stack_alloc` allocates `<size>` bytes in the current function's stack frame and
returns a `ptr` to the allocated storage. Native backends align the allocation to
the target stack requirement and restore the stack through the normal function
epilogue. This is the stable LowIR lowering for source builtins such as
`__builtin_alloca`; it is not a callable external symbol.

### Calls

```text
%t = call <type> @function(<arg-list>)
%t = call <type> <callee-value>(<arg-list>) as (<param-list>) -> <type>
call void @function(<arg-list>)
call void <callee-value>(<arg-list>) as (<param-list>) -> void
```

The PA13 subset requires direct calls and calls through pointer-valued operands.
For indirect calls, the explicit `as (...) -> ...` call signature is part of the
stable LowIR surface because the callee operand alone does not describe the
semantic boundary. That call signature may also carry call-boundary metadata
such as `arity=variadic`, `effects=readonly`, `unwind=no`, or `return=noreturn`
when needed.

### Terminators

```text
jump ^target
branch %cond, ^then, ^else
switch %selector, ^default, <case-value>:^case, ...
return void
return <type> <value>
```

`switch` is the first-class multi-way terminator form. It transfers control to
the first matching `<case-value>:^case` arm, or to `^default` if no arm matches.

## PA22 Exception/Runtime Instructions

PA22 extends the same LowIR family with explicit exception/runtime ABI instructions. These
forms are part of the defined long-term LowIR contract, and PA13 must already parse and
translate them so later milestones can target the same execution scaffold without extending
the PA13 adapter again.

### Handler Stack Management

```text
eh_try ^handler
eh_cleanup ^cleanup
eh_end
```

These instructions manage the dynamic exception-handler stack.

- `eh_try ^handler`
  pushes a catch-style handler that transfers control to block `^handler` if an exception is
  thrown
- `eh_cleanup ^cleanup`
  pushes a cleanup-style handler that transfers control to block `^cleanup`
- `eh_end`
  pops the most recently installed handler on the normal control-flow path

Handlers are lexically explicit in LowIR. The backend/runtime ABI decides how they are
represented at run time.

### Throwing and Resuming

```text
throw <type> <value>
resume
```

- `throw`
  raises a new exception carrying the lowered runtime value
- `resume`
  continues unwinding the currently active exception after a cleanup block has run

PA22 uses a simplified low-level ABI: the exception payload is a lowered scalar or pointer
value carried through the reserved runtime state.

### Accessing the Current Exception

```text
%t = exception <type>
```

This reads the current exception payload into a temporary of the given lowered type.

In the PA22 subset, `<type>` must be non-`void` and is expected to be a scalar or `ptr`
type already representable in LowIR.

## Structural Rules

- Every temporary must be defined before it is used.
- Every block must end in a terminator.
- Only terminators may transfer control.
- Stack slots represent addressable local storage.
- The text must be deterministic: the same input program should produce the same CY86 output.

## Later Reserved Extensions

The following are not required in PA13, but the IR is expected to grow to cover them without
changing its overall shape:

- field/member address operations
- construction and destruction actions
- virtual-call and vtable-related operations
- richer constant/data initializers
- template-instantiated function and object definitions
- exception-aware source-language lowering that targets the PA22 exception/runtime ABI

Some later-oriented forms are already required in PA13 because later lowering milestones need
the backend boundary to be in place before they start:

- object-lowered parameter/return ABI conventions
- `copyobj`
- `zeroinit`
- structured global data initializers
- the PA22 exception/runtime instructions:
  - `eh_try`
  - `eh_cleanup`
  - `eh_end`
  - `throw`
  - `exception`
  - `resume`

Those later operations should be added as new instruction/data forms in the same IR family,
not by replacing LowIR with a new backend boundary.
