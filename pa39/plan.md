# PA39 Inception Plan

Status: self-built PA1-PA6 pass; failure 12 (member-template bodies
bound inside an open class) fixed; the nsdecl rung (pa7) next runs
from the top.

PA39 adds no new compiler surface. The work is: make the existing
`../dev/cppgm++` rebuild every checkpoint tool from `frontend_source_sets.mk`
(`*-self`), keep the PA1-PA38 preservation ladder green under the self-built
checkpoints, then prove reproducibility (`*-self` rebuilds itself into a
byte-identical `*-inception`). Every ladder failure is treated as an earlier
compiler bug or a reproducibility bug until proven otherwise.

## Failure log (each fixed at its owning surface, oldest first)

The ladder initially failed in the very first rung: host-seeded
`../dev/cppgm++` could not compile four of the pptoken checkpoint
sources.

### Failure 1: "unterminated comment" (source_translation, pp_tokenizer, pptoken)

Comments in `dev/src/source_translation.h` (included by all three failing
TUs), `dev/src/source_translation.cpp`, and `dev/src/ctrl_expr.cpp` spell the
raw character sequence `\UFFFFFFFF` while documenting the end-of-input marker.
The course translation pipeline decodes universal-character-names during
phases 1-2, before comments are stripped, and `\UFFFFFFFF` wraps to the
end-of-input marker, cutting the file off inside a `//` comment.

This is **not** a compiler bug: `pptoken-ref` rejects the same file at the
same spot (`ERROR:9:73:Unterminated comment` on source_translation.h), so the
source text is invalid under the course dialect for any conforming course
compiler, host-seeded or self-built. Per the working rules, a source rewrite
is the correct fix exactly because the source is wrong under host-seeded
`../dev/cppgm++` (and the reference) too. Fix: reword the three comments so
they no longer contain a raw UCN spelling; behavior of the compiled code is
unchanged. A repo sweep found no other raw `\u`/`\U` + hex sequences in
`dev/`.

### Failure 2: "opaque unscoped enum declaration" (test_runner)

`dev/src/test_runner.cpp` includes `<fcntl.h>`; glibc's `struct f_owner_ex`
declares the member `enum __pid_type type;`. `DeclBinder::BindEnum`
(`dev/src/sema/decl_enum.cpp`, PA11 7.2 surface) treats every bodyless,
baseless, unscoped `DK_ENUM` as an opaque-enum-declaration and throws — but
an elaborated-type-specifier `enum E` used with a declarator merely refers to
a previously declared enumeration (7.1.6.3p3 / 3.4.4p2). `cppgm++-ref`
accepts `enum E e;` members, `enum E x = A;` declarations, and `enum E f();`
declarators, and compiles the `<fcntl.h>` probe; it still rejects standalone
`enum E;` (with or without a prior declaration) and elaborated references to
undeclared enums.

Real compiler bug, earliest owning surface PA11 (the enum entity model in
`sema/decl_enum.cpp`; the parse side already accepts the form). The PA7/PA8
`nsdecl` model has no enum support at all and none of its fixtures exercise
enums, so PA11 is the earliest surface that owns this behavior. Fix: give
`BindEnum` an `elaborated` mode (mirroring `BindClassForward`) that resolves
the name by unqualified lookup and requires an enumeration; wire it from
`BindNestedTypeSpecifier`. Reducer: new course tests under
`cppgm.tests/course/pa11/` with fixtures regenerated via
`make -C pa11 ref-test TEST=course/pa11/<test>.t`.

### Failure 3: PIE host links reject our objects (pptoken-self link)

With both compile failures fixed, the `pptoken-self` host link failed:
`ld` rejects our objects under the default-PIE host `g++`
(`R_X86_64_PC32 against __gxx_personality_v0 can not be used when making
a PIE object`, plus DT_TEXTREL warnings from movabs address
materialization). The reference compiler's objects link and run under
default PIE, so this is a real hosted-emission gap that the earlier
interop harness masked by always linking `-no-pie`
(`run_cpphostinterop_tests_worker.pl`). Three PA36-surface fixes, all
gated on host-object mode:

- `x86/lowir_to_mir_*`: taking the address of a function the unit only
  declares now loads from the GOT (`imported_function_global`),
  matching the host GOTPCREL spelling; direct calls keep PLT.
- `x86/mir_to_native.cpp`: OP_SYMBOL (defined-function) address movs
  materialize rip-relative like OP_GLOBAL, not movabs.
- `toolchain/elf_object.cpp`: the CIE personality is encoded
  indirect-pcrel (0x9b) through a DW.ref-style 8-byte .data slot
  holding the absolute `__gxx_personality_v0`, exactly the host
  convention. `toolchain/elf_reader.cpp` recovers the indirect form
  (verifying the slot really names `__gxx_personality_v0` so foreign
  personalities stay skipped), and the private runtime library defines
  `__gxx_personality_v0` so the slot resolves in private links.

Reducers: `cppgm.tests/course/pa36/link/700-hosted-imported-function-
addr-got-link-smoke` and `700-hosted-eh-personality-indirect-link-smoke`
pin the GOT spelling and the absence of a direct personality relocation
via `.inspect.expect`; the reference passes both.

Note: root `make test-strict` fails pre-existing (`--witness` is
rejected by the driver in emit modes for pa18-pa23); unrelated to and
unchanged by this work, and not part of the PA39 required checks.

### Failure 4: ctor-template specialization poisoned before its
### out-of-class definition (pptoken-self link, `_Hashtable` ctor)

With PIE fixed, `pptoken-self` linking failed on an undefined
`_Hashtable::_Hashtable<_InputIterator>(..., true_type)` — libstdc++'s
tagged range constructor, defined out of class. Two poisons in the
PA21 member-template surface (`sema/sem_member_template.cpp`):
candidate synthesis marked specs of definition-less ctor templates
`body_emitted` permanently, and `InstantiateCtorTemplateBody` marked
`body_emitted` before checking whether the pattern had a body. When a
class instantiation's out-of-class member definitions replay *after* a
member body already delegated into the tagged constructor (exactly
libstdc++'s ordering), the spec stayed body-less forever and the call
resolved to a never-defined weak symbol. Fixed by recording the demand
(`odr_used`, mirroring `OnSpecializationOdrUsed`'s use-before-
definition path) and binding demanded bodies when the definition
pairing completes. Also: the float/negmask constant pool addressed
memory with absolute `[disp32]` (`R_X86_64_32S`), rejected by PIE;
host objects now use rip-relative pool addressing (`PoolMem`).

Reducer: `cppgm.tests/course/pa36/link/600-hosted-unordered-set-init-
list-link-smoke` (the ordering needs hosted libstdc++, so the hosted
link suite is the earliest harness that can express it).

### Failure 5: loop-live values kept in caller-saved registers (pa1
### self tests, pptoken-self miscompiled)

`pptoken-self` built but failed every PA1 test, segfaulting or dropping
tokens after the first new-line. Reduced (freestanding `tok1.cpp`, then
raw LowIR): `MarkCallCrossings` in `x86/lowir_to_mir_analyze.cpp` used
the *linear* (def, last-use) window to decide `crosses_call`, so a
value defined before a loop and read at the loop head — with the loop's
call linearly *after* the last use — was housed in a caller-saved pool
register (r8) and clobbered every iteration. Latent since PA28, exposed
by PA37's -O2 LowIR CSE hoisting `index`-derived addresses out of
loops; mode-independent (whole-program and hosted alike). The reference
backend houses such values callee-saved. Fix: extend each value's
effective range through every loop region (backedge target start →
backedge block end, to fixpoint) it is live into; the old behavior is
unchanged for values that cross no backedge, so the pinned PA28/PA38
MIR fixtures stay byte-identical (report re-run green).

Reducer: `cppgm.tests/course/pa28/300-loop-live-value-across-call.t`
(behavior kind: raw LowIR loop with the call in the loop body and the
linearly-last use before it).

### Failure 6: parked call results left unnormalized (pa1 self tests,
### `*=` scanned as two non-whitespace characters)

`pptoken-self` next failed recognizing every punctuator: the inlined
`IsOpStartChar` switch compared 64-bit against a frame home that the
call-result commit had written with a 32-bit store (`SpellType(i32)`),
leaving stale upper bytes — exposed when -O2 slot promotion replaced
`load i32 $c` with the raw call temp. The backend's contract is that
integer register/frame homes hold the canonical normalized 64-bit form
(readers `load.i64`, the reference emits `sext.i32` when parking a call
result, and host callees leave upper bits undefined). Fixes in
`x86/lowir_to_mir_flow.cpp`: pool-parked and frame-parked sub-64
integer call results normalize via `emit_narrow_normalize` (frame
stores now i64), and `LowerSwitch` re-normalizes sub-64 selector and
case temps after staging. Pinned MIR fixtures unchanged (no pinned
sub-64 selector shapes); report re-run green.

Reducer: `cppgm.tests/course/pa28/300-call-result-switch-selector.t`
(negative i32 call result crossing a second call, then switched on).

### Failure 7: synthesized members of extern-instantiated classes
### suppressed (posttoken-self link)

PA1 now passes self-built. `posttoken-self` linking failed on the
defaulted `allocator<char>::operator=` odr-used by `__alloc_on_move`
(string move-assignment): `bits/allocator.h` declares
`extern template class allocator<char>`, and the PA36 host-mode
suppression turned all inline members of extern-declared
specializations into external references. The host toolchain lowers
synthesized special members inline and never materializes them
(libstdc++ exports no `allocator<char>::operator=`), so such a
reference can never resolve. Fix in `lowering/lower_unit.cpp`:
synthesized (implicit/defaulted) special members stay locally emitted
under the suppression.

Reducer: `cppgm.tests/course/pa36/link/600-hosted-string-move-assign-
link-smoke`.

### Failure 8: reference-bound temporaries destroyed at statement end
### (pa2 self tests: map::at, double frees)

`posttoken-self` linked but every literal path corrupted memory: a
class prvalue bound to a reference declaration (including range-for's
hidden `__range`) was destroyed with the full expression, so
`for (const IntegerType& c : IntegerTypeCandidates(...))` iterated
freed memory. The reference compiler extends but never destroys (a
leak the fixtures cannot see); ours destroyed early — 12.2p5 requires
scope-lifetime destruction. Fix: `ExtendBoundTemporaryLifetime`
(sema/sem_lifetime.cpp) marks the directly-bound class prvalue
`lifetime_extended` (materialization keeps `needs_dtor`; the two
statement-cleanup registrations in lowering/lower_member.cpp skip
extended nodes) and attaches a scope-exit destructor to the
declaration through the stored reference; the reference-declaration
lowering registers it as an ordinary scope cleanup. Wired for plain
reference declarations and the range-for `__range` binding;
namespace-scope extension (shutdown-time destruction) remains out of
scope, matching the reference emission.

Reducer: `cppgm.tests/course/pa36/link/600-hosted-ref-extended-
temporary-runtime-smoke` (value-correctness only, so the reference —
which leaks but reads correctly — generates passing fixtures).

### Failure 9: glvalue conditional arms raw-copied (pa2 self tests,
### floating literals double-freeing)

`cut ? source.substr(...) : source` lowered the lvalue arm in place as
a raw object copy of the nontrivial string, aliasing the source's heap
buffer. 5.16p6 converts glvalue arms of a class-typed prvalue
conditional to prvalues (copy-initialization); nontrivially-copyable
glvalue arms now wrap in their copy construction in
`AnalyzeConditional` (sema/sem_expr.cpp), while trivially copyable
classes keep the pinned raw-copy arm shape. Reducer:
`cppgm.tests/course/pa36/link/600-hosted-string-conditional-copy-
runtime-smoke`.

### Failure 10 (frontier): aggregate elements with array members
### (pa6 recog checkpoint, parse_expr.cpp)

Self-built PA1-PA5 all pass. The recog checkpoint fails compiling
`dev/src/parse/parse_expr.cpp`: `kBinaryLevels` is an aggregate array
whose elements carry an ARRAY member (`ETokenType ops[4]`) initialized
from a braced sub-list. The synthesized aggregate-constructor
machinery's member initializer does
`CopyInitialize(value, field.type, "member initializer")`
(sema/sem_class.cpp:~1180) which has no braced-list-into-array-member
handling — "no conversion for member initializer". Pre-existing for
locals too (not introduced by the namespace-array routing fix).
Reducers ready in /tmp/pa39probe/agg3.cpp (namespace) and agg4.cpp
(local): a `struct { const char* name; T ops[4]; int num; }` array
with `{ "or", { T_A }, 1 }` elements. The in-place single-object path
(`ConsumeAggregateItems`/`ConsumeArrayItems`) already handles array
members; only the synthesized-constructor form (arrays of aggregates)
lacks the contract.

Confirmed reference contract (cppgm++-ref --emit-lowir on both
reducers): the synthesized aggregate ctor keeps the ARRAY type in its
signature (mangles `A4_4ETok`) but passes it as `ptr [pass=decay]`
(`decay` is already in the LowIR validator's kPasses); the caller
materializes each braced sub-list into an `$argarr__N` slot exactly
like our existing `LowerLocalArrayInit` shape (byte-identical on a
plain local array) and passes its address; the ctor body raw-copies
`copyobj <span> <param>, <member>`. The reference emits a dead
member-address recompute after the copyobj; whole-program shapes
already legitimately diverge (alias vs ov2 bodies, function order), so
this is a behavior fix, not a byte-parity one.

Fix (earliest owning surface, the PA24-era aggregate ctor machinery):
- `EnsureAggregateCtor` keeps array field types unadjusted in the
  signature; the body's parameter reference is typed pointer-to-element
  (the decayed spill slot holds a pointer).
- `MemberAssignAction` accepts an array member from a matching decayed
  pointer (no CopyInitialize; the lowering raw-copies).
- `LowerMemberAssignment` gains a TK_ARRAY branch: member address,
  pointer value, `copyobj`.
- `LowerAbiParameter` maps TK_ARRAY to `ptr [pass=decay]`.
- `AppendAggregateArrayInit` routes a braced sub-list aimed at an array
  parameter through `AnalyzeBracedInit` (SN_BRACED_INIT_LIST), and
  zero-fills omitted array members with an empty braced list;
  `LowerCallArgument` materializes SN_BRACED_INIT_LIST for array
  parameters into an `argarr` slot via `LowerLocalArrayInit` (the
  existing reference-to-array shape, minus the reference binding).
- `MakeAggregateTemporary` gets the same omitted-tail handling.

### Failure 11: narrow parameter frame homes read at 64 bits
### (pa6 recog checkpoint, ParseBinaryExpression segfault)

With the aggregate fix in, recog-self built and linked but segfaulted
running pa6 tests/130-postfix.t (`2+int('a')` — any matched binary
operator). `kBinaryLevels[level]` computed a corrupted element address:
the high 32 bits of `level` were stale pointer bytes. Root cause in the
PA28 MIR staging (`dev/src/x86/lowir_to_mir_*`): `SpillParamHome` parks
a narrow (i32) parameter into its 8-byte frame home with a narrow
store (high half = stack garbage), while `gpr_read` and
`emit_dest_copy`'s VL_FRAME branch read parameter homes at 64 bits —
here for the frontend's pinned subscript shape `binary mul i64
%level, 32`, whose sext the shape leaves implicit. The reference's
contract (probed via `lowir2native-ref --dump-machine-ir`, and pinned
by pa28 structural fixtures for named slots): narrow parks stay
narrow, and consumers re-load at the value's own width and
re-normalize (`load.i32; sext.i32`). Fix: reader-side — `gpr_read`
and `emit_dest_copy` re-load an is_param VL_FRAME value at its own
width and `emit_narrow_normalize` when the home is narrower than the
consumer; temp homes keep the widened normalized-64 contract
(pinned). The lazy `resolve_location` park keeps the narrow spelling.
Reducer: `cppgm.tests/course/pa28/300-narrow-param-home-across-call.t`
(dirties the stack, then six crossing i32 params exhaust the
callee-saved pool; verified failing before the fix, passing after).

### Failure 12: member-template bodies bound while the enclosing
### class is still open (pa7 nsdecl checkpoint, all sema TUs)

Recog-self passes pa6. The nsdecl rung failed earlier: host-seeded
`../dev/cppgm++` rejected all seven `sema/*.cpp` checkpoint TUs with
"class assignment is outside the PA12 assignment boundary". Reduced
(header bisection to `sema/type.h`, then freestanding) to
`struct A { std::vector<A> elems; };` — a member field whose class
template is specialized over the still-open class. Completing `A`
completes `vector<A>` (`RecordMemberField` →
`EnsureTypeCompleteness`), whose specialization replay eagerly binds
member bodies; those odr-use member-template specs (`_M_insert_aux<A>`
et al.) before the out-of-class `vector.tcc` definitions replay, and
the definition merge (`InstantiatePendingFunctions`, sem_spec.cpp)
bound the demanded bodies immediately — analyzing
`*__position = std::move(__x_copy)` (a class assignment on `A`)
while `A` had no implicit `operator=` yet. The reference defers such
bodies past the forward pass (14.6.4.1p3), and the architecture
already has the queue: `OnSpecializationOdrUsed` pushes to
`pending_instantiations_` when `instantiating_` is set, drained at
end of unit when every class is complete.

Fix (earliest owning surface, the PA21 member-template machinery,
`sema/template_body.cpp`): `InstantiatePendingFunctions` mirrors
`OnSpecializationOdrUsed` — while `instantiating_`, a demanded
non-constexpr body queues to `pending_instantiations_` instead of
binding at the merge; constexpr patterns still bind immediately
(constant evaluation may need them before the drain). The end-of-unit
drain is failure-tolerant, binding order unchanged for every existing
fixture (report re-run green).

Reducer: `cppgm.tests/course/pa36/link/600-hosted-member-template-
open-class-body-runtime-smoke` (verified failing before the fix,
passing after). A pa21 exact-LowIR fixture cannot express it: the
whole-program emission order and low display names for this shape
legitimately diverge from the reference (poison/retry sequencing,
`helper_Node_` vs `helper` display spelling, synthesized-`operator=`
copyobj vs memberwise form), while the `object=` manglings match — so
the value-correctness link smoke is the earliest harness that can pin
the behavior, like failures 8 and 9. The ctor-template merge path
(`sem_member_template.cpp`, failure 4) still binds at the merge; if a
ladder failure reduces to a ctor-template body bound inside an open
class, give it the same deferral through its `DeferredBody` route.

## Validation plan

1. `make -C pa39 probe-self-object SOURCE=...` on each previously failing TU.
2. Reducer tests pass; `make test-report-through-pa38` stays green.
3. `make -C pa39 test-through-pa10 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++`
   (the blocker) — iterate on the next first failure this exposes, same
   method: reduce, find owning surface, fix there, add reducer.
4. `make -C pa39 compare-pptoken-inception ...` as the first reproducibility
   compare, then the full `compare-cppgm++-inception` target.

## Ladder expectations

Self-built checkpoints must behave identically to host-seeded builds on the
same inputs; >5x slowdowns, timeouts, or OOM in `*-self`/`*-inception`
compiles are layer divergence to trace back to a miscompiled self compiler,
not something to tune around. No self-hosting special cases, no generated
source discovery, no harness weakening.
