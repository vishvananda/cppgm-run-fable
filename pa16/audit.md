# PA16 Audit

## Audit Plan

Scope: the PA16 value-semantics slice — everything between the PA15 audit
commit (`33d1d0c8b`) and HEAD under `dev/src`, excluding the upstream
assignment-export commit (`b3f14259f`, merged as `0ec93c8ed`) which added
provided scaffold headers (`mir_model.h`, `x86_register_model.h`,
`ir_symbol_model.h`, `lowir_model.h` growth) and audit-script updates.
Baseline at audit start: `make test-report-through-pa16` green (1152/1152),
`cppgm_file_audit.pl --stage pa16 --paths dev/src` passes with the one
pre-existing `parser.h` warning untouched since PA6.

### Files to inspect

Sema (new):
- `sema/sem_special.cpp` (791) — demand synthesis of copy/move ctors and
  assignments, bit-field unit copies, trivial storage-prefix `copyobj`
- `sema/sem_new.cpp` (347) — new/delete semantics, builtin allocation decls
- `sema/sem_cast.cpp` (244) — explicit conversions, sizeof/alignof (audit
  split from sem_expr)

Sema (grown):
- `sema/sem_class.cpp` (+320) — implicit special-member declaration at
  CompleteClass, deleted-participation facts, conversion-operator recording
- `sema/sem_expr.cpp` (+407), `sem_lifetime.cpp` (+316) — value-transfer
  wrapping, temporary materialization, cleanup arming
- `sema/sem_operator.cpp` (+235) — built-in operator candidates, ADL for
  ordinary calls, ref-qualified implicit-object binding
- `sema/sem_convert.cpp` (+143) — conversion-function ranking vs converting
  ctors, reference-binding preference
- `sema/class_info.h/.cpp` (+227) — ctor kinds, triviality queries
- `sem_binder.cpp/.h`, `sem_ctor.cpp`, `sem_member.cpp`, `decl_binder.cpp`,
  `scope.h`/`scope_lookup.cpp` (fn_owner, using-import addressing),
  `type*.cpp/.h` (ref-qualifiers)

Lowering (new):
- `lowering/lower_new.cpp` (404) — new/delete lowering, array count headers,
  nothrow null branches
- `lowering/lower_convert.cpp` (259) — value conversion emission (audit
  split from lower_expr)

Lowering (grown):
- `lower_function.cpp/.h` (+277/+72) — class ABI split (direct obj vs
  by_address/indirect_result), return-slot reuse, callee-owned param dtors
- `lower_expr.cpp` (467 changed), `lower_member.cpp` (+283) — call-site
  argument materialization, `copyobj` transfers, xvalue results
- `lower_unit.cpp` (+138) — demand emission of synthesized specials,
  elided-copy odr-use sweep
- `lower_types.cpp/.h` — boundary classifier over ClassInfo

Parser: `ast_parse_class.cpp` (ref-qualifiers, out-of-class specials),
`ast_parse_expr.cpp` (new/delete), `ast_parse_decl.cpp`, `ast.h`.

Plan discrepancy to resolve: `plan.md` names `lowering/lower_value.cpp` as a
new file; it does not exist. Locate where class-ABI classification and value
transfer lowering actually landed and reconcile the plan text.

### What I will look for

1. **Cheating / shortcut paths**: test-specific or source-shape acceptance
   gates (string matches on identifiers/file names/test shapes), dummy or
   minimal LowIR output, fallback success paths masking analysis failures,
   interpreter/VM/trampoline/embedded-payload substitutes for real lowering,
   timeout workarounds, skipped phases.
2. **Regressions**: weakened earlier-stage checks; PA12 dump or PA14/15 LowIR
   drift (gated by the through-pa16 report); previously-rejected code now
   silently accepted (e.g. deleted/inaccessible specials not diagnosed on the
   supported paths).
3. **Stringly semantic facts / ownership**: lowering re-deriving overload,
   triviality, or ABI facts from names or storage types instead of typed
   ClassInfo/SemNode facts; call sites and definitions classifying the class
   ABI independently in ways that could disagree; slot-name string parsing;
   duplicated triviality logic between sema and lowering.
4. **Performance risks**: per-call linear scans over all classes/scopes
   (quadratic in program size); repeated full-SemUnit walks per demanded
   helper in `lower_unit.cpp`; hot-path recomputation of layout, triviality,
   or mangled names; excessive copying of ClassInfo/candidate vectors in
   overload resolution; ADL collector cost per ordinary call.
5. **File-audit discipline**: implementation bodies in grown headers
   (`sem_binder.h` +78, `sem_expr.h` +68, `lower_function.h` +72,
   `lower_program.h`); splits that exist only to dodge size limits
   (`sem_cast.cpp`, `lower_convert.cpp` must be cohesive units, not
   fragments); code moved to unchecked paths.

### Ownership boundaries to verify (from plan.md)

- sema owns overload resolution, special-member declaration/synthesis state,
  triviality facts, and the action tree; typed facts on SemNodes.
- lowering owns slot naming, ABI shapes, `copyobj` emission, return-slot
  reuse; call sites and definitions must agree by consulting one classifier.
- `class_info` owns layout; triviality queries are free functions over
  ClassInfo links, consumed (not re-derived) by lowering.

### Method

Parallel read-only sweeps over the diff (cheat hunt, ownership/stringly,
performance, file-audit/header discipline, regression), each finding verified
by direct reading — and, for behavior claims, by compiling probe programs
with the audited binary and diffing against the pinned reference compiler
(`pa16/cppgm++-ref`) — before any fix. Every confirmed blocker fixed in this
pass, then re-gated with `make test-report-through-pa16` and the pa16 file
audit.

## Findings

Every behavioral finding below was reproduced with probe programs against
`pa16/cppgm++-ref` before fixing, and re-probed after. "Ref-verified parity"
means the probe output matches the reference (modulo the two documented
conventions the checked-in refs pin our way: body-derived `unwind=no`
metadata and the EH-runtime declares that accompany emitted dispatch
regions).

### Blockers (all fixed)

1. **Synthesized assignment byte-copied members with user `operator=`**
   (`sem_special.cpp` `TrivialStoragePrefix`): the trivial storage prefix for
   copy/move *assignment* consulted copy/move *constructor* triviality, so a
   member with a trivial copy ctor but user-provided `operator=` was
   swallowed into the whole-object `copyobj` and its `operator=` never ran.
   Wrong code on valid input (probe: member-wise `M::operator=` in ref, raw
   `copyobj 8x4` in ours).
2. **Call-result temporaries bypassed destructor synthesis and checks**
   (`sem_expr.cpp` `CallResult`/`AnalyzeConditional` + `lower_member.cpp`
   `MakeResultCleanup`): sema pinned only a bare `needs_dtor` bool; lowering
   fabricated the `~C` callee from `"~" + name`, skipping
   `EnsureImplicitDtor` and the deleted/access checks. An implicit `C::~C`
   was *declared strong but never defined* (undefined symbol; ref emits the
   weak synthesized definition plus the D2 alias).
3. **Explicit `p->~C()` fabricated the callee the same way**
   (`sem_member.cpp` `MakeExplicitDestructorCall`): same missing synthesis
   and checks inside sema.
4. **By-value class arguments destroyed twice** (`lower_expr.cpp`
   `MaterializeClassArg` → `MaterializeTemporary`): the caller registered a
   cleanup for the materialized argument object while the callee also
   destroys its by-value parameter (the pinned callee-owned convention,
   `AttachParameterDtor`). Ref emits no caller-side destruction.
5. **Unbounded mutual recursion in conversion classification**
   (`sem_convert.cpp` `ClassifyValueConversion`): converting-constructor
   parameters were classified with full user-conversion recursion; the
   `CR_USER` rejection ran only after the recursion returned, so
   `struct A { A(const B&); }; struct B { B(const A&); };` made any overload
   resolution over an `A`/`B` parameter non-terminating (and acyclic
   ctor chains cost O(m^k) per argument).
6. **Unmemoized recursive class facts on per-expression paths**
   (`class_info.cpp`): the five triviality queries,
   `NeedsDestruction`/`NeedsConstruction`, and the two `*HasEffects`
   predicates re-walked the whole base/field subobject tree per query, and
   PA16 calls them per expression, per call boundary, and per rendered
   declare — O(expressions × subobject-tree size), exponential on nested
   class shapes.
7. **Effect-free destructor elision over-applied to temporaries**
   (`sem_member.cpp` `MakeTemporaryObject`, `sem_expr.cpp` `CallResult`,
   `sem_new.cpp` `AnalyzeDelete`; regression vs PA15/ref from commit
   92392d47a): the ref destroys class temporaries and `delete` objects
   whenever the class needs destruction, even when the chain is effect-free
   (`struct S { S(int){} ~S(){} }; S(1);` emits the call, the weak `@S___S`
   definition, and the alias). Only *named locals* and the
   *braced-init-list assignment RHS* elide effect-free destruction
   (both ref-probed). Our gate on `DestructionHasEffects` silently dropped
   those cleanups — a monotonic-extension violation against still-valid
   PA15 programs.
8. **Ambiguous operator overloads silently lowered as the built-in form**
   (`sem_operator.cpp` `ResolveOperatorCall`): the catch around
   `SelectBestOverload` treated *ambiguous* the same as *no viable* and fell
   back to the built-in operator (13.3.1.2p3 violation; ref errors, ours
   exited 0).
9. **EH dispatch regions armed by temporaries that never produce caller
   cleanups** (`lower_member.cpp` `TreeHasTempCleanups`): any `needs_dtor`
   node in the full expression pre-armed unwind dispatch, wrapping calls in
   `eh_try` regions (plus the runtime declares) for expressions whose only
   destructible temporary is a callee-owned argument or the final call
   result. Ref-probed contract: a full expression arms only when some call
   executes while a caller-owned temporary is live.
10. **Ordinary stores to bit-fields were no-ops** (`lower_member.cpp`
    `LowerBitFieldAssignment`; pre-existing before PA16, found by the
    regression sweep's probe matrix): the store path read
    `bit_width`/`bit_offset` from the *assignment* node, which only the
    constructor member-initializer path stamps; ordinary `x.a = -3` saw
    width 0 and emitted `and old, -1` / `and 0, value` — writing the old
    value back unchanged. No checked-in test covers out-of-ctor bit-field
    stores, which is how it survived.
11. **Two conversion functions no better than each other silently picked
    declaration order** (`sem_convert.cpp` `ClassifySourceConversionFunction`;
    ref-verified: `S s; t(s);` with `operator int`/`operator long` to a
    `short` parameter is rejected by the ref, accepted by ours via
    `operator int`). 13.3.3.1p10: the ambiguous user-defined sequence makes
    the source non-viable for that destination.
12. **Own + using-directive-imported function names rejected instead of
    forming an overload set** (`scope_lookup.cpp`, 7.3.4p6 last sentence;
    introduced by fb8628b04 in this slice): `namespace D { int f(int); }
    using namespace D; int f(char); f(7);` threw a same-level ambiguity
    that the call fallback then masked as `undeclared name f`. The ref
    resolves the union and calls `D::f(int)`. (The pre-PA16 behavior was
    worse — it silently called the wrong overload.) Related: the ambiguity
    error itself was swallowed by `AnalyzeCall`'s not-found recovery, so any
    genuine 7.3.4p6 ambiguity surfaced as a misleading `undeclared name`.

### Checked and clean (verified, no change needed)

- **No cheating**: no test-identifier or source-shape gates, no dummy or
  hardcoded output, no embedded payloads or interpreter substitutes, no
  timeout dodges, no `.ref` reads; every "outside the PA16 assignment
  boundary" path is an honest hard failure mapped to `EXIT_FAILURE`.
- **One ABI classifier**: `LowerAbiParameter`/`LowerAbiReturn`
  (`lower_types.cpp`) over `ClassParamDirect`/`ClassReturnDirect`
  (`class_info.cpp`) is the only direct/by_address/indirect_result decision
  point; definitions, declares, direct calls, indirect-call signatures, and
  result materialization all consume it — call sites and definitions cannot
  disagree by construction.
- **Ownership boundaries hold**: layout math only in `class_info.cpp`;
  lowering consumes typed SemNode facts (`member_offset`, `synth_copy`,
  `trivial_copy`, `elided`) and never re-runs lookup; synthesized-special
  emission is keyed by typed `(scope, name, type, special)` entries that
  sema registered.
- **12.8p32 move-then-copy fallback on `return local;`** catches broadly
  (deleted move falls back to copy) — ref-verified as the course-pinned
  behavior, kept as is.
- **Class-scope allocation functions**: `new`/`delete` consult only the
  global scope; ref-verified parity (the reference also calls the builtin
  global `operator new` for a class declaring its own) — course-accepted
  subset behavior, no change.
- **File audit**: both in-range audit-script commits strictly tighten
  (compressed-line and hosted-header detection); all grown headers are
  declaration-only; the `sem_cast.cpp`/`lower_convert.cpp` splits are
  cohesive topical units; all five new files registered in
  `frontend_source_sets.mk` and covered by the audit; no code outside
  `dev/src`; the only warning (`parser.h`) predates PA15.
- **Accepted performance shapes** (documented, not defects): the
  demand-sweep rescan floor (PA15-reviewed design; re-scans are flag checks);
  per-call-site `FunctionRef` string keys (pre-PA16 convention, linear);
  compile-time unrolling of fixed-bound class-array construction (the shape
  the checked-in refs pin; array new/delete correctly use runtime loops);
  the 7.3.4p6 same-level scan (zero-cost without using-directives); ADL
  collection bounded by argument types, not program size; mangled names
  cached at entry creation; `NodeMayThrow` computed once per body and
  consumed as stored facts.
- **Presentation-keyed conventions kept** (PA15-precedent: the spelling is
  the semantic identity, output is presentation-only): `MangleTerminalName`
  operator-string parsing; conversion-function member bindings keyed by
  `"operator " + DescribeType(result)` (uniquified displays keep distinct
  types distinct); lowering-internal `pass=`/`C1/C2/D1/D2` string tags
  (produced and consumed by lowering with total mappings); builtin
  allocation declares re-keyed by canonical name in `lower_unit.cpp`
  (sema declares the same canonical shapes).
- **Ref-binary divergences pinned our way by checked-in refs**: body-derived
  `unwind=no` on synthesized/inline bodies (suppresses EH regions the older
  ref binary still emits) and the EH-runtime declares that accompany any
  emitted dispatch region; bit-field store mask constants render in the
  declared type's value space (`-32` vs the ref binary's `4294967264`) and
  reads use `ushr`+mask — same bits, our established convention, no
  checked-in ref pins otherwise.

## Changes Made

All blockers above were fixed in this pass:

1. `TrivialStoragePrefix` passes the assign/ctor axis through
   `TransferTrivial` for both the base and member checks (`sem_special.cpp`).
2. `SemNode` gained a typed `result_dtor` slot (deep-cloned): `CallResult`
   and `AnalyzeConditional` resolve the temporary's destructor through
   `MakeTemporaryDtor` (running `EnsureImplicitDtor` + deleted/access
   checks) and pin the action; `MaterializeClassResult` consumes it and the
   lowering-side `MakeResultCleanup` fabrication was deleted
   (`sem_node.h/.cpp`, `sem_expr.cpp`, `lower_member.cpp`,
   `lower_function.h`).
3. `MakeExplicitDestructorCall` resolves through the binder first
   (`sem_member.cpp`).
4. `MaterializeTemporary` takes an explicit `register_cleanup`;
   `MaterializeClassArg` passes false — by-value argument objects are
   callee-owned (`lower_member.cpp`, `lower_expr.cpp`).
5. Conversion classification threads `allow_user` through
   `ClassifyValueConversion`/`ClassifyReferenceBinding`/
   `ClassifyConversionImpl`; nested classifications inside a user conversion
   run standard-only (13.3.3.1.2p1), which both preserves outcomes and
   bounds the recursion (`sem_convert.cpp`).
6. The nine recursive class facts are memoized per `ClassInfo` under a
   global facts version; out-of-class special-member binding (the only
   post-completion mutation of their inputs: `BindQualifiedSpecialMember`'s
   dtor/ctor definition attach, out-of-class `= default`, and
   `RecomputeUserCtorFact`) bumps the version via `InvalidateClassFacts`
   (`class_info.h/.cpp`, `sem_class.cpp`, `sem_special.cpp`).
7. Temporary/delete destruction now keys on `NeedsDestruction`;
   `MakeTemporaryObject` takes `braced_assign` so only the braced-assignment
   RHS keeps the ref-pinned effect-free elision (the resolved destructor is
   still access-checked there); `AnalyzeDelete` always attaches the resolved
   destructor (`sem_member.cpp`, `sem_expr.cpp`, `sem_cast.cpp`,
   `sem_new.cpp`, `sem_expr.h`).
8. `SelectBestOverload` throws a typed `NoViableOverloadError`
   (declared in `sem_convert.h`; also used by `ResolveClassConstructor`'s
   empty-set path); `ResolveOperatorCall` falls back to the built-in form
   only on that type, so ambiguity propagates (`sem_convert.h/.cpp`,
   `sem_class.cpp`, `sem_operator.cpp`).
9. `BeginFullExpression` arms unwind dispatch via an evaluation-order scan
   (`ScanArmsCleanups`): a call event after a caller-owned destructible
   temporary is live arms the expression; callee-owned by-value argument
   objects and trailing result temporaries no longer arm alone
   (`lower_member.cpp`, `lower_function.h`).
10. `LowerBitFieldAssignment` reads the bit facts from the member lvalue for
    ordinary assignments (the assignment node keeps them only on the
    constructor-initializer path) (`lower_member.cpp`).
11. `ClassifySourceConversionFunction` tracks ties between candidates no
    better than each other; an unresolved tie makes the sequence non-viable
    (a strictly better later candidate clears it) (`sem_convert.cpp`).
12. Same-level function bindings merge into one overload set:
    `UnqualifiedLookup` collects them (own binding first, dedup by shared
    type node) and either fills a caller-provided `fn_set` or throws the new
    typed `AmbiguousLookupError`; `AnalyzeCall` retries lookup with the set
    and routes it through `AnalyzeAdlCall`, which now seeds from multiple
    visible bindings; qualified lookup keeps its single-binding contract;
    non-function ambiguity propagates instead of masking as
    `undeclared name` (`scope_lookup.h/.cpp`, `sem_expr.h/.cpp`,
    `sem_operator.cpp`).
13. `DemandTreeCallees` walks each synthesized definition at most once
    (`demanded_trees_` on `LowerProgram`) (`lower_unit.cpp`,
    `lower_program.h`).

## Validation

- `make test-report-through-pa16`: 1152/1152 pass after every fix cluster
  and at the end.
- `perl scripts/cppgm_file_audit.pl --stage pa16 --paths dev/src`: passes
  (one pre-existing `parser.h` warning from PA6).
- Probe matrix vs `pa16/cppgm++-ref` (all now matching, modulo the two
  documented ref-binary conventions): synthesized-assignment member
  `operator=`; implicit-dtor synthesis for discarded call results;
  by-value argument destruction (no caller-side calls); effect-free
  temporary destruction for functional casts, ref-bound arguments,
  discarded results, and `delete`; braced-assignment elision retained;
  EH arming for ref-bound temporaries (2-region shape), none for
  callee-owned/trailing-result shapes; mutual-conversion termination;
  ambiguous operator and ambiguous conversion-function rejection;
  own+imported overload-set call resolving to `D::f`; variable same-level
  ambiguity still rejected; bit-field store masks semantically correct.
