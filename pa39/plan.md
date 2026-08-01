# PA39 Inception Plan

Status: **PA39 COMPLETE.** `make -C pa39 compare-cppgm++-inception`
passes: the self-built compiler recompiles every checkpoint tool and
the compiler itself, all 136 objects byte-match the host-seeded
build, and cppgm++-inception is byte-identical to cppgm++-self
(`MATCH cppgm++`). compare-pptoken-inception matches, test-through-
pa10 is green (pa10 134/134 self-built), `make
test-report-through-pa38` is green (3456/3456 at completion,
3461/3461 after the audit added five reducers), and the file audit
passes. The post-completion audit (pa39/audit.md and the "Audit
fixes" section below) fixed six more behavioral defects and re-ran
the whole ladder against the audited compiler. Twenty-seven failures
were root-caused and fixed on the way to inception; the last two
were backend codegen bugs the sema TUs exposed:

- Failure 26: a scratch parameter copy hoisted into the prologue
  could target r8/r9 after a call-staging evacuation released the
  hold, clobbering the still-live incoming 5th/6th argument
  (pool_clobbered_ now set at every parameter pool grant and at the
  hoist itself; reducer
  `cppgm.tests/course/pa28/210-sret-six-gpr-param-forwarding.t`).
- Failure 27: eh_end popped the synthetic throw-payload window
  instead of the region it closes, leaking the class-throw
  allocation's region onto the marker stack and double-running
  cleanup pads on unwind (reducer `cppgm.tests/course/pa36/link/
  600-hosted-class-throw-payload-region-runtime-smoke.{t,t.1}`).

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

### Failure 13: elaborated-type-specifier first-declarations land in
### the current scope (pa7 nsdecl checkpoint, sema/type.cpp)

With failure 12 fixed, `sema/type.cpp` failed "no matching function
for call to IsStdInitializerListTemplate": `sema/type.h` first
declares `TemplateInfo` through the member
`const struct TemplateInfo* template_entity` (TemplateArg, line ~96),
and `BindClassForward`'s elaborated create path declared the entity in
`current_` — the class scope — making a `TemplateArg::TemplateInfo`
distinct from the `sema/template_info.h` definition, so every later
use failed to unify (reduced: `struct Holder { const struct Late* p; };
struct Late { int v; };` — assigning `&x` to `h.p` reports "no
conversion"). 3.3.2p6: a class first declared by an
elaborated-type-specifier inside a declaration is declared in the
smallest enclosing namespace or block scope, past class, prototype,
and template-parameter scopes. Fix in `sema/decl_binder.cpp`
(`BindClassForward`, PA11 surface): the elaborated create path walks
`current_` up to the nearest SCOPE_NAMESPACE/SCOPE_BLOCK and declares
there (MA_PUBLIC when rehomed); standalone forward declarations keep
the current scope.

No course reducer can express this fix: every reference generation
mishandles the construct — `cppgm++-ref` (final) rejects the
function-parameter spelling outright ("unsupported namespace-scope
declarator") and binds the member spelling to a class-scope entity
(`pointer to const struct Holder::Late`), and the pa12-era ref does
the same — so ref-generated fixtures in any harness would pin the
wrong behavior. Per the testing rules the refs are not perfect and
the standard governs non-test inputs; the nsdecl ladder rung (which
compiles `sema/type.h`'s exact member and parameter spellings) and
the inception build gate the behavior. The host dialect here is a
strict superset of the reference dialect: forms the ref accepts still
bind identically (report re-run green).

### Failure 14: addr-of-slot temps dropped from stack-passed call
### arguments (pa7 self tests, nsdecl-self segfault)

nsdecl-self built and linked, but segfaulted on the first variable
declaration (`char c;`): `Program::LinkEntity` wrote `created = true`
through a null reference. The caller (`DeclParser::LinkNewEntity`,
7 GPR-class arguments, the 7th on the stack) staged the stack argument
as `store.i64 [rsp], rax` with stale rax — the argument was
`%t = addr $created`, a `VL_SLOT_ADDR` temp (rematerialized at each
use), and the integer-class stack-argument path in
`x86/lowir_to_mir_flow.cpp` had no `VL_SLOT_ADDR` case: the fallback
read `location.reg` off a location that has none. The register-class
argument path already handled it. Host-seeded `../dev/cppgm++`
reproduces directly (7-arg method call with a `bool&` local as the
7th argument segfaults at -O0); the reference backend stages
`lea r11, [slot]; store.ptr [rsp], r11`. Fixes (PA28 surface):
- `lowir_to_mir_flow.cpp`: the integer-class stack-argument path gains
  the `VL_SLOT_ADDR` branch — lea through r11, `store.ptr` (the
  reference shape).
- `lowir_to_mir_program.cpp` (`PlanWideParam`): the stack-parameter
  intake copy spells a full-eightbyte pointer parameter `ptr` instead
  of `i64` (the reference load.ptr/store.ptr shape); container chunks
  stay i64.

Reducer: `cppgm.tests/course/pa28/300-slot-addr-stack-argument.t`
(fails before the fix, passes after; `.ref.mir` checked in to pin the
exact reference staging). The callee keeps its register parameters
unused so the pinned parking discipline (which legitimately differs
from the reference for late single-use register params) stays out of
the fixture.

### Failure 15: class names hidden by same-scope functions lost to
### elaborated lookup (pa9 cy86 checkpoint, x86/elf_program.cpp)

pa7 and pa8 pass self-built. The cy86 rung failed compiling
`x86/elf_program.cpp` and `cy86.cpp`: "redeclaration of stat". glibc's
`<sys/stat.h>` declares `struct stat` then the function `stat`;
`SemBinder::BindFunctionName`'s 3.3.10p2 branch hides the class by
*replacing* the SB_TYPE binding with the function binding, so a later
`struct stat*` elaborated-type-specifier found no type (3.4.4p2 must
ignore non-type names), took the first-declaration path, and
`AddBinding` collided with the function binding. Fix (PA11/PA12
binder surface): `ScopeBinding` gains `hidden_type`; the 3.3.10p2
replacement records the hidden class there, and
`DeclBinder::BindClassForward`'s elaborated path falls back to an
unfiltered lookup and resolves through `hidden_type` (union-key
checked) before first-declaring. Variable-hides-class and
class-declared-after-function orderings remain unhandled until a
checkpoint source needs them.

Reducer: `cppgm.tests/course/pa12/elaborated-class-hidden-by-function`
(--emit-semantics; fails before the fix, passes after). The reducer
spells `(*buf).x` rather than `buf->x`: the reference presents `->`
member access without the deref wrapper we synthesize (5.2.5), a
presentation divergence no existing fixture covers and separate from
this bug.

### Failure 16 (fixed): eager noexcept-spec evaluation
### instantiates traits inside open classes (pa10 rung, 6 TUs:
### type_builder, ast_parse_class, template_info, template_args,
### sem_pack, sem_builtin_template)

Repro: `struct A { std::vector<A> elems; int v; };` plus a function
calling `list.push_back(item)` — "static_assert failed \"template
argument must be a complete class or an unbounded array\"".

Chain (gdb trap on the assert, full backtrace verified): completing
`A` completes `vector<A>` (`RecordMemberField` →
`EnsureTypeCompleteness`); the member replay composes declarators and
`TypeBuilder::ApplyDeclaratorSuffix` (type_builder.cpp:~567)
eagerly evaluates each member's `noexcept(expr)` specifier via
`EvaluateNoexceptSpec`. The spec expressions contain `noexcept(call)`
operators whose candidate deduction (`AppendTemplateCandidates` →
`EnsureFunctionSpecialization`) composes further declarators with
their own specs, recursively, until a template-id value argument reads
`is_constructible<A, A>::value` — `LookupConstant` →
`EnsureTypeCompleteness` instantiates the trait's class body and its
`__is_complete_or_unbounded` static_assert evaluates with `A` still
open. The failure is swallowed (`EvaluateNoexceptSpec` returns
false), but the trait specializations stay memoized with poisoned
members; the first legitimate post-completion use (push_back's
drained body via `move_if_noexcept`) hits the poison and the lowering
reports the stored instantiation error. g++/the reference never
evaluate these specs at replay time (CWG 1330: exception specs of
members of class-template specializations instantiate only when
needed), so their traits instantiate post-completion and pass.

Design (validated against fixture constraints, not yet implemented):
- Gate: a class-replay depth counter set around
  `InstantiateSpecializationBody`'s class bind. When composing a
  member declarator under replay, do NOT evaluate a FQ_NOEXCEPT
  has_expr spec; record the unevaluated `{expr, current_ scope}` (the
  alias scope keeps template-parameter bindings alive) in the
  composed `DeclaratorInfo` (new pending fields).
- Consumers that persist `composed.noexcept_simple` into durable
  records store the pending entry instead: `ScopeBinding` per-overload
  (decl_binder.cpp:~160), `ClassCtor.unwind_no`/`dtor_unwind_no`
  (sem_class.cpp:342/373), `spec->self.fn_unwind_no`
  (template_deduce.cpp:1366).
- Resolution must be ON-DEMAND at the read sites (sem_call.cpp:289,
  sem_apply.cpp:399, sem_operator.cpp:554/731, sem_member.cpp,
  sem_new.cpp:69, sem_special.cpp:973): evaluate in the recorded
  scope, memoize into the record. An end-of-unit patch pass is NOT
  enough: forward-pass call analysis of green tests reads
  `fn_unwind_no` immediately after completion, and a
  deferred-until-patch value would flip pinned `[unwind=no]` LowIR
  EH shapes. On-demand reads post-completion give today's values for
  every green case; reads inside the bad window only occur from
  replay-bound bodies, which already poison-and-retry.
- Body-node flags (`MakeInstantiatedBodyNode`, sem_member_body:165)
  re-compose at body-bind time; drained bodies bind post-completion,
  so evaluating there (replay depth 0) is correct and keeps the
  pinned unwind flags on instantiated bodies.

Implemented as designed (`class_replay_depth_` gate in
`InstantiateSpecializationBody`; `DeferNoexceptSpecScope()`
composition hook; per-overload/ctor/dtor pending records resolved
on-demand at every unwind-fact read site). Fixing the deferral
unmasked four more real bugs in the same demand chain, each fixed at
its owning surface:

- `InstantiateClassFromPartial` (PA19/PA21, sem_spec.cpp) had no
  reset-on-failure, unlike the primary path: a partial-matched trait
  whose base clause faulted mid-window (`__is_move_insertable
  <allocator<A>>` reaching `is_move_constructible<A>` with `A` open)
  stayed a hollow "instantiated" record forever. It now mirrors the
  primary reset - and to keep deduction probes tolerant, a spec whose
  bind already failed reports softly on re-demand
  (`ClassSpecialization::bind_failed` in
  `InstantiateSpecializationBody`): the first failure keeps the hard
  14.8.2p8 fault, repeats read like the old hollow record (probes
  legitimately compose candidates whose alias targets never
  instantiate, e.g. `tuple_element<0, tuple<>>`).
- `ConstBodyRegistry` (PA20, const_eval.cpp) indexed `unit_.deferred`
  by append-only watermark; the end-of-unit poisoned-body retry
  replaces nodes in place, leaving the index dangling on the
  destroyed poison and blind to the healed body
  (`_S_use_relocate()`'s template-argument evaluation then failed as
  "undefined function"). `RetryDeferredBodies` now invalidates the
  index after each replacement.
- The pending member-class completion (PA21, sem_template.cpp) erased
  its record before the bind: a mid-window failure
  (`vector<A>::_Temporary_value`) was unretryable. The record now
  restores on failure with the entity reset.
- `RetryDeferredBodies` dropped failed retries: entries retry in
  poison order, not dependency order, so a body reading a sibling's
  constexpr definition that heals later stayed poisoned. Failures now
  re-queue while any body heals (fixpoint; a no-progress round ends
  the loop as before).

Reducers: `cppgm.tests/course/pa36/link/601-hosted-self-element-
vector-runtime-smoke` (vector<A> member of open A, push_back +
insert) and `602-hosted-self-element-map-unique-ptr-runtime-smoke`
(map<string, unique_ptr<T>> member bodies in a class template held by
its own element class).

### Failure 17 (fixed): undeclared name __bool_constant (pa10 rung:
### sem_spec, sem_template)

The misleading message came from `stl_tree.h`'s
`_M_move_assign(__x, __bool_constant<_Node_alloc_traits::
_S_nothrow_move()>())`: the alias template-id's value argument spells
a zero-argument constexpr static member call behind a
typedef-spelled qualifier, which parses as a *function type-id*
(`Q::f ()` vexing parse - the parser's type oracle sees the typedef
qualifier as a type prefix, unlike a direct class-name spelling that
resolves the terminal to a function). The value re-read
(`EvaluateZeroArgConstantCall`, PA20 template_arg_const.cpp) only
handled deduced member-template specializations; an ordinary
(non-template) member fell through to false, the callee type never
resolved, and the call misrouted to ADL - "undeclared name". Fix at
the PA20 surface: the re-read now evaluates the unique
zero-parameter overload through the full constant engine over its
analyzed definition (a synthesized SN_CALL over the resolved
binding). The retry fixpoint above is load-bearing here: in-window
demands poison and heal at the end-of-unit rounds.

Reducer: `cppgm.tests/course/pa36/link/603-hosted-typedef-qualified-
constexpr-call-arg-runtime-smoke` (bool_c<node_traits::
nothrow_move()>() selecting sizeof-dependent overloads; prints "1 2").

### Failure 18 (fixed): i128 bitnot and variable shift counts
### (pa10 rung: const_expr)

`sema/const_expr.cpp` (the PA20 const-eval engine's SWide/UWide
helpers) needs two i128 forms the PA29 wide lowering lacked:
- `~wide`: the frontend spells complement `bitnot` (the narrow path's
  spelling; `not` is the logical form), but the wide unary case only
  accepted `neg`/`not` — and implemented "not" AS the complement. The
  branch now accepts `neg`/`bitnot`.
- `wide >> count` with a runtime count ("i128 shift count must be
  constant"): `LowerWideVariableShift` lowers branchless — the 64-bit
  half shifts use count%64 (the hardware cl masking), the cross half
  gates out via an and/neg/sbb mask when count%64 == 0, and a second
  mask selects the count>=64 arrangement (with sign fill for
  arithmetic shifts). Validated byte-identical against g++ output for
  all 128 counts of shl/lshr/ashr on positive and negative patterns
  at -O0 and -O3.

Reducer: `cppgm.tests/course/pa36/link/600-hosted-wide-variable-
shift-runtime-smoke` (prints all 513 results; fails before the fix,
passes after). The pa28 whole-program harness cannot express it for
the reference, which lowers these shifts to libgcc's `__ashlti3`
family — unresolvable in a whole-program link — so the hosted suite
is the earliest harness. Noted for later: our hosted i128 by-value
parameter ABI (stack) differs from SysV's rdi:rsi pair — self-
consistent across our own objects, and no checkpoint source passes
i128 across the host boundary, but a g++-compiled caller cannot call
an i128-by-value function we compiled.

### Failure 19 (fixed): eh filter maps to conflicting catch types
### (pa10 rung: template_spelling, sem_spec)

The PA37 inliner (`lowir_opt_inline.cpp`) only refused EH-bearing
callees *inside* a protected region; at depth 0 it pasted a small
callee with its own try/catch (template_spelling.cpp's SpellBail
helpers) into a caller with different EH. eh_catch selectors are
function-local ids also baked into the pad's dispatch compares as
plain literals, so they cannot be renumbered at paste time - and the
LSDA requires per-function-unique filter/type pairs, so the merged
function collided on selector 1 at object emission. Fix at the PA37
surface: a callee with any EH instruction never pastes.

Also fixed on the same chain: the qualified (out-of-class) special
-member definition path (`BindQualifiedDestructor` etc.,
sem_class.cpp) analyzed instantiated bodies without the
poison-and-retry wrapper the in-class path has, so `~_Hashtable`'s
completeness static_assert - reading a deferred noexcept fact
conservatively while still in-window - failed the whole instantiation
instead of poisoning one body (the unordered_set hosted smokes caught
this). The three sites now route through
`AnalyzeQualifiedMemberBody`.

Reducer: `cppgm.tests/course/pa36/link/604-hosted-inlined-try-catch-
selector-runtime-smoke` (small internal try/catch callee called at
depth 0 by a caller with a different catch type; fails object
emission before the fix, prints "10 94 9" after).

### Failure 20 (fixed): source parameter named `ret` collides with
### the indirect-result slot (pa10 rung: sem_lambda)

Host-seeded `../dev/cppgm++` rejected `sema/sem_lambda.cpp`:
"LowIR validation error: duplicate parameter %ret in
@SemBinder__BindLambdaBody". `BindLambdaBody(..., const TypePtr& ret)`
returns a nontrivial class, so the lowering synthesizes the
`%ret : ptr [pass=indirect_result]` slot - and spelled the source
parameter `%ret` too. Every validated pipeline (`-c` objects AND the
whole-program executable path, both of which round-trip the LowIR
text through ParseLowIRProgram/ValidateLowIRProgram) rejects the
duplicate. Fix at the PA14-era lowering surface
(lowering/lower_function.cpp): a source parameter whose name is
already taken in the signature (the reserved `ret`) takes a
de-conflicted register name (`ret__argN`, mirroring the NewTemp
collision discipline), while its storage slot and the
address-alias/slot-map keys keep the source spelling (body references
resolve by source name).

No course reducer can express this fix: the reference shares the bug
in every validated mode (`cppgm++-ref -c` fails with "duplicate
storage name %ret" on the reduced form), so ref-generated fixtures in
the -c harnesses (pa31+) would pin the failure, and the pa15-pa27
`--emit-lowir` text harnesses compare against reference dumps that
spell the ambiguous `%ret, %ret` form (which their unvalidated dump
path accepts silently) - matching those would require keeping the
bug. Like failure 13, the refs are not perfect here; the pa10 ladder
rung (sem_lambda.cpp) and the inception build gate the behavior.
Reduced form kept for reference: a nontrivial class returned by value
from a function whose parameter is named `ret`.

### Failure 21 (fixed): frontend retains transient template
### resolution scopes for the whole unit (pa10 rung OOM kills)

The rung's Error-137/125 exits are memory, not logic: the Ralph check
runs in a 64 GiB memory cgroup, the selfhost object build runs at
-j32 (DEFAULT_BUILD_JOBS), heavy sema TUs peaked at ~3.4 GiB each,
and toolchain/compile_unit.cpp (10.2 GiB) and cppgm++.cpp (8.4 GiB)
individually exceeded the 8 GiB per-command RSS cap. g++ compiles the
same TUs in ~250 MB, and RSS grows linearly across the whole compile
- the retain-everything shape. Massif attribution of the 3.4 GiB peak
(sem_member_template.cpp): ~1.8 GiB (>50%) is Scope + ScopeBinding
records created per template-id *resolution* and retained forever by
the TypesModel - argument alias scopes (ResolveAliasTemplateId's
substitution context), per-resolution argument-binding scopes
(EnsureArgBindingScope), deduction-probe scopes (FillDeducedDefaults,
failed EnsureFunctionSpecialization SFINAE probes leaking param/
capture scopes), and pack-element scopes. ~650 MB is SemNode bodies
(needed by the lowering; not transient).

Fix (PA18/PA21-era template machinery + the scope model):
`TypesModel::ReleaseScope` frees one transiently-created scope
(detach from parent's child list, swap-remove from storage);
`TransientScope` RAII guards release at every probe/resolution exit.
Safety-by-refusal: the release refuses the global scope, member
scopes (entity set), pinned scopes (a deferred noexcept spec's
recorded evaluation context pins its chain - the CWG 1330 records
hold scopes durably), and any scope that acquired children. Scope
dumps only print in the pa10/pa11-era --emit-types mode, which
predates instantiation, so released instantiation scopes are
dump-invisible. Result: sem_member_template.cpp frontend peak 3.43
GiB -> 2.26 GiB (-34%) with byte-identical emitted LowIR.

### Failure 22 (fixed): hidden-friend overloads hide the ordinary
### function template from qualified lookup (pa10 rung: lowir_opt_cse,
### lowir_opt_fold, lowir_opt_cfg)

With sem_lambda fixed the rung reached the lowir TUs and failed
"no member named swap": any TU instantiating `std::vector` lost
qualified `std::swap`. libstdc++'s `_Bit_reference` (stl_bvector.h)
declares three concrete hidden-friend `swap` overloads; merging them
into the namespace binding that until then carried only the ordinary
`swap` function template filled every concrete `fn_adl_only` slot
with true, and `HiddenFriendOnly` (scope_lookup.cpp) hid the whole
binding - the ordinary template's visibility had no accounting once
concrete slots existed. Fix at the PA18/PA22 friend-visibility
surface: `ScopeBinding::fn_templates_adl_only` tracks the template
set's visibility (set when the templates were declared only by friend
declarations, cleared by any ordinary template declaration), and
`HiddenFriendOnly` keeps the binding visible while an
ordinarily-declared template lives under the name.

Reducer: `cppgm.tests/course/pa18/hidden-friend-keeps-template-
visible.t` (qualified call through the template beside two concrete
hidden friends, plus an ADL call reaching the friend; fails
"no member named swap" before the fix, passes with ref-generated
fixtures after).

### Failure 23 (fixed): members of non-lvalue objects classified as
### lvalues (cppgm++-self host link: copying unique_ptr push_back)

With every TU compiling, the cppgm++-self host link failed:
sem_binder.o carried an undefined reference to
`__new_allocator<unique_ptr<SemNode>>::construct<unique_ptr,
const unique_ptr&>` inside an emitted
`vector<unique_ptr<SemNode>>::push_back(const&)` body - the COPYING
overload of a move-only element, which g++ never instantiates for
this source. `AnalyzeExpandedParenInit` passes
`ZeroValue(...).node` (a member of a prvalue) to push_back; 5.2.5p4
makes E1.E2 an xvalue when E1 is not an lvalue, so overload
resolution must select `push_back(&&)`. `AnalyzeMemberAccess`
classified every data-member access VC_LVALUE regardless of the
object's category, the const& overload won, and its
deleted-copy-instantiating body poisoned into a declare-only
reference (the failure-tolerant end-of-unit drain) instead of a
diagnostic. Fix at the PA12 member-access surface (sem_member.cpp):
a non-reference member of a non-lvalue object is VC_XVALUE
(mirroring the existing member-pointer-access and member-call object
adjustments); reference members stay lvalues.

Reducer: `cppgm.tests/course/pa12/member-of-prvalue-xvalue.t`
(--emit-semantics pins `member-expression xvalue` through a
static_cast<V&&> object beside the lvalue contrast; ref-generated
fixtures). Noted while reducing: the reference presents
reference-member access with the reference type spelled in the member
expression and prints synthesized destructors for temporary-bearing
translation units at pa12 - presentation divergences no fixture pins,
kept out of the reducer.

Latent issue documented (not fixed here): a hard failure inside a
demanded body can survive the end-of-unit retry as a declare-only
emission, turning an ill-formed program into an undefined reference
at link time; a diagnostic would be better once demanded-versus-
speculative body demands are distinguished.

### Failure 24 (fixed): if-condition declarations' cleanups leak into
### the enclosing scope (pa10 tests: cppgm++-self segfault on any
### non-identifier token)

cppgm++-self linked, but --emit-ast on `int x = 1;` freed a garbage
pointer inside `AstParser::ParseUnaryExpression`: the shared final-
return tail destroyed the if-condition variable `contextual` on paths
that never constructed it. `LowerIf` (and `LowerSwitch`) opened no
cleanup scope for an SN_CONDITION_DECLARATION, so
`RegisterCleanup` parked the variable's destructor in the ENCLOSING
scope's list - every later exit of that scope ran it, including the
outer if's false path in the braceless
`if (k) if (Ptr p = make()) return p;` nesting (and paths where the
variable's own if had already destroyed it). Fix at the PA14/15
statement-lowering surface (lowering/lower_function.cpp): LowerIf and
LowerSwitch bracket the whole selection statement with
Push/PopCleanupScope when the condition is a declaration, mirroring
LowerFor's init-statement scope; returns inside the branches keep
destroying the variable through the ordinary active-scope walk
(6.4p3). while/do condition declarations (per-iteration destruction)
remain unhandled until a checkpoint source needs them - none does.

Reducer: `cppgm.tests/course/pa36/link/600-hosted-if-condition-decl-
scope-runtime-smoke` (all three paths of the nesting print their
selected values; free(): invalid pointer before the fix, 20/10/30
after; ref-generated fixtures).

### Failure 25 (fixed): expression-level branches destroy the
### enclosing full expression's temporaries (pa10 tests:
### cppgm++-self double free printing special members)

With parsing fixed the rung reached 15/134 pa10 tests, then
cppgm++-self died "double free detected in tcache 2" inside
`operator+(string&&, string&&)` from `PrintSpecialMember`: the
LowIR showed `branch %cond, ^cond_true_cleanup, ^cond_false_cleanup`
edges destroying the operator+'s LEFT operand (a heap string
temporary created earlier in the same full expression) before the
conditional's arms even ran - append then reallocated an
already-freed buffer. `BranchOnValue`'s per-edge cleanup trampolines
(the statement-condition contract: an if/while condition IS a full
expression, so its temporaries die on the branch edges) fired for
EXPRESSION-level branches too (?:, &&, ||), where the enclosing full
expression continues past the join and 12.2 keeps its temporaries
alive to its end (the reference branches these directly, no edge
blocks). Fix at the PA16/PA25-era lowering surface: BranchOnValue
takes `edge_cleanups`; the statement path (LowerCondition's tail)
keeps the trampolines, the conditional-value/void/class-init/
conditional-address and short-circuit sites pass false. The
short-circuit RHS-arm temp handling (dies inside its arm, the pinned
12.2 conditional-operand shape) is untouched.

Reducer: `cppgm.tests/course/pa36/link/600-hosted-conditional-
operand-temp-lifetime-runtime-smoke` (long heap strings so SSO
cannot mask the free; double free before, correct concatenations
after; ref-generated fixtures).

### Failure 26 (FIXED): thunk-shaped callers clobber the r9 argument
### via a prologue-hoisted scratch parameter copy

test-through-pa10 was green, but compare-pptoken-inception failed
immediately: every cppgm++-self compile segfaulted (Error 139).
Reductions, smallest first (all pass under host-seeded
`../dev/cppgm++`, all break under `cppgm++-self`):

- `pa15/tests/general/100-default-member-initializer-class-member.t`
  (struct X { Y y = Y(); }) trips libstdc++'s
  "unique_ptr operator*: get() != pointer()" assertion inside
  `SemBinder::NodeMayThrow` recursing over a children vector holding
  a NULL entry, reached from EnsureImplicitDefaultCtor <-
  MakeConstructorCall <- AppendClassDefaultInit. Found by walking the
  per-assignment suites with the self binary
  (`make -C pa39 test-pa11..` : pa11 49/49, pa12 126/126, pa14 68/68
  pass; pa15 fails at test 5).
- `#include <bits/exception_ptr.h>` (or <exception>, <stdexcept>)
  with an empty main segfaults destroying a vector<ConversionSource>
  in AnalyzeStaticMethodCall (valgrind: _M_release on control block
  0x10 - a small-constant-corrupted shared_ptr), via
  ResolveDecltype of the is_destructible-style
  `decltype(__test<_Tp>(0))` trait shape (freestanding form in the
  session notes: struct Probe { template<typename T, typename =
  decltype(T().~T())> static TrueType test(int); ... };
  typedef decltype(Probe::test<X>(0)) Result;).

Signature across crashes: owning handles (SemNodePtr children,
TypePtr control blocks) destroyed twice or left null mid-tree -
the same cleanup-discipline family as failures 24/25 but in code the
HOST compiler emitted for the sema TUs. Localization so far:
replacing single suspect objects with -O0/-O1 rebuilds (sem_ctor,
sem_binder, sem_class, sem_special, sem_node) does NOT fix the pa15
reducer, so the miscompiled function is either level-independent or
lives in another TU / a weak template instantiation (single-object
replacement cannot displace comdat copies stamped into many TUs -
the failure-24 lesson). Bulk g++-object mixing is blocked by an
abi-tag mangling divergence (our objects reference
string-returning free functions like FlattenName/FundamentalTypeName
without g++'s [abi:cxx11] tag - itself a latent host-interop gap
worth a look). By-value unique_ptr/vector<unique_ptr> parameter
passing and the bare DMI shape both pass host-compiled runtime
reducers, so the trigger is more specific than either.

Root cause (found without any TU bisection): gdb frame-walking the
pa15 reducer recovered the corrupted tree - the temporary Y()'s
constructor-call node held a junk child pointer that was never a
SemNode (conditional `break _ZdlPv`/ctor breakpoints proved no
SemNode ever lived or died at that address). Register dumps at
`MakeConstructorCall` entry showed the junk call arrived with
r9 == rdx: the `SemNodePtr()` null-temp argument's invisible
reference had been replaced by the ClassInfo pointer. The call goes
through the ZThn interface thunk (SemExprAnalyzer::MakeTemporaryObject
calls host_.MakeConstructorCall), and the thunk's own body -
sret + this + rdx/rcx/r8/r9 + one stack argument, generated by our
backend when the host compiler built the sema TUs - was miscompiled:

- PlanGprParam granted the r8/r9-homed parameters their own incoming
  registers as pool homes (self-copies that secure nothing);
- at the forwarded call, the staging evacuated the endangered r8/r9
  values to anonymous frame slots and released the pool holds;
- the rdx parameter's VL_PENDING_COPY then materialized, scanned the
  pool, found r9 "free and unclobbered", and hoisted `mov rdx, r9`
  into the prologue - upstream of the evacuation spill, so the spill
  saved rdx's value and the callee reloaded it as argument six.

The invariant was already spelled out in resolve_location's comment
("only registers no already-emitted code has written can carry it"),
but the parameter pool grants never set pool_clobbered_. Fix: set
pool_clobbered_ at both parameter pool-grant sites (PlanGprParam and
the call-crossing loop) and at resolve_location's own grant (a second
hoist into the same register would clobber the first copy's live
range). Reducers: `pa28/tests/behavior/210-sret-six-gpr-param-
forwarding.t` (raw-LowIR forwarding function, ref-generated
fixtures; exits 1 before the fix, 0 after) plus a C++ probe
(secondary-base virtual override with the full GPR signature) that
failed at -O0 before and passes at -O0..-O3 after. pa28 suite stays
green (34/34 strict, 58/58 structural, 15/15 behavior, 4/4 course) -
no pinned MIR fixture shifted, because the flag only forbids hoists
that were previously unsound. Why nothing earlier caught it: the
shape needs a base-adjusting thunk whose target takes sret plus four
more integer-register parameters AND a caller whose argument sources
sit in pending-copy homes - the sema interface (ISemHost at base
offset 128 in SemBinder) is the first place the ladder ever executed
that combination.

### Failure 27 (FIXED): eh_end popped the synthetic throw-payload
### window instead of the region it closes

After failure 26, the pa15 DMI reducer passed but
`#include <bits/exception_ptr.h>` still segfaulted the self compiler:
`vector<ConversionSource>::~vector()` ran TWICE on the same
`sources` local in AnalyzeStaticMethodCall (gdb: two ~vector calls
from two different return addresses, both landing pads; between the
two destructions the only EH event was one `_Unwind_Resume` - no
throw, no catch - so the resume re-entered a second cleanup pad of
the same frame). Diagnosis without any self-build iteration: run the
TU's own `--emit-lowir -O3` text through `dev/lowir2native` with
temporary env-guarded traces in LowerResume/EhRegionForArming; two
resumes carried live regions (`dispatch_57` covered by `dispatch_55`,
`dispatch_69` by `dispatch_67`), and the arming trace showed the
region armed around `__cxa_allocate_exception` still on the abstract
stack at the throw's own arming.

Root cause in `lowir_to_mir_eh.cpp` (SimulateEhBlock): a
`role=eh_allocate_exception` call pushes a synthetic throw-payload
window, popped again at the `role=eh_throw` call. But the CLASS-type
throw lowering closes the allocation's region before the payload
constructor runs (`eh_try ^A; alloc; eh_end ... eh_try ^B; ctor;
throw; eh_end`), so that `eh_end` popped the synthetic window instead
of region A - region A leaked onto the marker stack for the rest of
the function, later flat pads' resumes were covered by it, and the
unwinder re-entered its pad and destroyed the enclosing locals a
second time. Fix: `eh_end` retires any synthetic windows above the
region it closes (the throw-site pop still serves the unsplit scalar
shape). After the fix every resume in the function maps to -1
(terminal flat pads) or to the intended catch-cleanup chain pad.
Reducer: a class-type `throw E("..." + name)` from a function with
destructible locals, caller catching - double destruction before,
exactly-once after, at -O0 and -O3 (`cppgm.tests/course/pa36/link/
600-hosted-class-throw-payload-region-runtime-smoke.{t,t.1}`,
ref-generated fixtures). Note the trigger needs the throw OUTSIDE any
try/catch in the throwing function (in-frame handler routing masks
the leak), destructible locals so the allocation call gets its own
region, and a class exception with a nontrivially-built payload.

## Validation plan

1. `make -C pa39 probe-self-object SOURCE=...` on each previously failing TU.
2. Reducer tests pass; `make test-report-through-pa38` stays green.
3. `make -C pa39 test-through-pa10 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++`
   (the blocker) — iterate on the next first failure this exposes, same
   method: reduce, find owning surface, fix there, add reducer.
4. `make -C pa39 compare-pptoken-inception ...` as the first reproducibility
   compare, then the full `compare-cppgm++-inception` target.

Status (this pass): report green 3451/3451 after failures 20-22;
same-binary object determinism spot-checked across runs (type.cpp,
sem_spec.cpp byte-identical under ASLR). Memory after the failure-21
work: heavy sema TU 3.43->2.26 GiB frontend peak, compile_unit.cpp
10.2->4.6 GiB, cppgm++.cpp 8.4->3.9 GiB (both giants now under the
8 GiB per-command cap), plus token-stream release before sema and a
post-frontend malloc_trim. The canonical -j32 rung then completed
inside the 64 GiB envelope (test-through-pa10 green, both selfhost and
inception object trees built with no EXIT_OOM), so the further levers
considered at the time - shrinking the SemNode record (592 B x ~1.1M)
and the ScopeBinding record (896 B, mostly empty per-overload vectors
on non-function bindings) - were not needed for PA39.

## Audit fixes (post-inception)

The PA39 audit (pa39/audit.md) re-reviewed the whole change range and
fixed six behavioral defects the ladder had not exposed, each at its
owning surface and each verified failing-before/passing-after:

- Deferred noexcept facts now reach definition nodes bound inside a
  class replay window (the CWG-1330 machinery's own gap: the callee
  compiled without the 15.4p9 terminate barrier its call sites
  assume). Reducer: pa36/link 605 terminate smoke.
- `emit_dest_copy` adopts `gpr_read`'s width rule: narrow parameter
  homes re-load at their own width and re-normalize unconditionally;
  temp homes load the canonical i64 (a negative `int` shifted or
  divided out of an EH-forced frame home folded wrong in both modes).
  Reducer: pa28 210-narrow-signed-shift-div-frame-home.
- The hidden-friend merge path clears `fn_templates_adl_only`
  (an ordinary template redeclaration beside a concrete hidden friend
  stayed invisible). Reducer: pa18 hidden-friend-template-redeclared-
  beside-friends.
- Reference lifetime extension skips static/thread_local declarations
  (the failure-8 fix had regressed `static const T& = temp` into a
  whole-program boundary error and a wrong hosted atexit target); no
  ref-fixture harness can express the shape (audit.md finding 4).
- Statement-condition `&&`/`||` no longer destroy left-operand
  temporaries on the short-circuit fall-through edge (per-edge cleanup
  flags through LowerCondition/BranchOnValue). Reducer: pa36/link 607.
- Class-static member aggregate arrays take the explicit
  element-address init form (their `@__cppgm_init` clones previously
  mislowered). Reducer: pa36/link 606.

Plus: `bind_failed` pairs with its record reset, the end-of-unit retry
loop tracks drain progress, lifetime-extended results rebalance their
EH region, `WideReadPair`/`value_info` fail loudly on desyncs, the ELF
personality writer reuses defined symbols and the reader verifies the
legacy 0x1b form, and rehomed elaborated declarations display their
true scope. The full disposition of every review finding - including
the claims that did not verify and the measured non-issues - is in
pa39/audit.md.

## Architecture Review

- **No PA39 compiler surface.** The inception work added zero
  self-hosting conditionals to `dev/src`: a sweep for
  inception/selfhost/PA39 gating finds only comments tagging fixes to
  their motivating failure. Every one of the 27 ladder failures was
  fixed at the earliest assignment surface that owns the behavior
  (PA11 enum/elaborated binding, PA12 member categories, PA14-16
  lowering scope/branch cleanups, PA18/22 friend visibility, PA20
  const-eval, PA21 member-template sequencing, PA28 MIR staging, PA29
  wide ops, PA36 hosted emission, PA37 inliner), so the self-build and
  ordinary user programs exercise the same code paths.
- **Fixed source sets, two flavor trees.** Checkpoint composition
  comes only from the hand-maintained
  `dev/frontend_source_sets.mk` lists (unchanged in the PA39 range;
  the Makefile errors out on a missing list rather than scanning).
  The selfhost and inception trees compile the same sources with
  byte-identical command lines from the same directory; the only
  variable is which compiler binary runs. The inception pass
  byte-compares every object against its selfhost twin before the
  final link compare, so drift is caught at the first responsible TU.
- **Reproducibility model.** Cross-flavor identity is what PA39
  proves: two different compiler binaries (host-seeded-built vs
  self-built) produce 136 byte-identical objects and a byte-identical
  final binary, which also transitively validates emission-order
  determinism in every stage the compiler runs on itself. Generated
  configuration (`cppgm_builtin_host_config.h`) is produced once,
  content-compared on regeneration, and shared by both flavors. The
  `-DCPPGM_DEFAULT_*` defines carry flavor-invariant values (and are
  currently consumed by no source file, so they cannot embed paths in
  objects). Same-binary determinism across runs was spot-checked under
  ASLR during the ladder (type.cpp, sem_spec.cpp byte-identical).
- **Harness integrity.** The PA39 range changes no test harness: no
  edits under `scripts/`, the root `Makefile`, or any other
  `pa*/Makefile`; the cppgm.tests diff is purely additive (new
  reducers and their reference-generated fixtures). The pa39
  timeouts (30s text tests, 900s/3600s compile walls) and the 8 GiB
  RSS cap are the course-provided values from the original assignment
  export, not run-added accommodations.
- **Memory discipline as architecture.** The failure-21 work gave the
  template machinery a transient-scope ownership rule:
  resolution/probe scopes are released at their creation site
  (`TransientScope` RAII over `ReleaseScope`), and release refuses by
  construction anything durable (global scope, member scopes, pinned
  scopes such as deferred-noexcept evaluation contexts, any scope
  that acquired children). That keeps the CWG-1330 records' pinned
  chains alive while stopping the retain-everything growth that made
  the giant TUs exceed the per-command cap.
- **Reducer coverage.** Every behavioral fix left a test at the
  earliest harness that can express it (pa11, pa12, pa18, pa28
  course tests; pa36/link smokes where hosted libstdc++ ordering or
  host linking is intrinsic to the trigger). The two exceptions
  (failures 13 and 20) are documented in their failure entries with
  the specific reason a reference-generated fixture would pin the
  wrong behavior; both are gated by the ladder rungs and the
  inception build itself.

## Final Architecture Review

Completed after the audit fixes landed and the full ladder re-ran
against the audited compiler.

- **The inception property held under change.** The audit modified 30+
  compiler files (sema fact plumbing, lowering cleanup edges, backend
  width discipline, ELF EH edges) and the ladder was re-run from
  scratch: host report green (3461/3461 with the five new reducers),
  self-built checkpoints through pa10 green, pptoken inception and the
  full cppgm++ inception byte-identical again. No pinned fixture was
  regenerated or weakened at any point - every fix either matched the
  reference on a previously-uncovered shape or changed emission only
  where no fixture pins it (verified suite-by-suite before the ladder).
- **Fix-at-owning-surface discipline preserved.** The audit fixes
  follow the same rule as the 27 ladder failures: each landed in the
  earlier assignment surface that owns the behavior, with a reducer in
  the earliest harness that can express it; the one inexpressible fix
  (static-reference extension) carries the same class of written
  justification as failures 13 and 20.
- **The CWG-1330 machinery is now internally complete.** The deferral
  had three fact sinks (scope bindings, ctor/dtor records, definition
  nodes); the audit closed the third. All three resolve promote-only,
  and the resolution points are ordered: read sites on demand
  post-window, definition nodes after the end-of-unit retry fixpoint,
  both strictly before the lowering consumes them.
- **Backend width contract is now one rule.** Narrow parameter homes:
  load at own width, normalize unconditionally. Every other GPR frame
  home: load the canonical normalized i64. `gpr_read` and
  `emit_dest_copy` share the rule's single implementation
  (`emit_frame_home_load`), the compare path documents why its
  unsigned-only exception is sound, and the checked `value_info` read
  makes a future desync loud instead of silently skipping
  normalization.
- **No deferred problems.** Every audit finding was either fixed or
  resolved as not-a-defect with recorded evidence (audit.md); nothing
  is parked as future work. The remaining documented boundaries
  (namespace/static-duration reference extension, per-iteration
  while/do condition destruction, class-element aggregate argument
  arrays, i128 host-boundary ABI) predate the audit, reject loudly or
  match the reference's own emission, and are exercised by no
  checkpoint source - they are dialect scope, not defects.

## Ladder expectations

Self-built checkpoints must behave identically to host-seeded builds on the
same inputs; >5x slowdowns, timeouts, or OOM in `*-self`/`*-inception`
compiles are layer divergence to trace back to a miscompiled self compiler,
not something to tune around. No self-hosting special cases, no generated
source discovery, no harness weakening.
