# PA18 Audit

## Audit Plan

Scope: all changes since the PA17 audit (`db7df0650..HEAD`), ~5.6k
insertions across sema and lowering, gated by
`make test-report-through-pa18` (1385/1385 at audit start) and
`perl scripts/cppgm_file_audit.pl --stage pa18 --paths dev/src`.

### Files to inspect

New template machinery (primary review target):

- `dev/src/sema/template_info.h` / `template_info.cpp` — data model,
  specialization cache keys, argument spellings.
- `dev/src/sema/sem_template.cpp` — capture, class instantiation
  driver, out-of-class member instantiation, explicit instantiation.
- `dev/src/sema/template_deduce.cpp` — deduction, substitution,
  partial-ordering tie-break.
- `dev/src/sema/sem_template_check.cpp` — definition-time sanity walk
  (must stay conservative, no source-shape acceptance).
- `dev/src/sema/sem_binder.h` (+222) — instantiation context
  save/restore, binder state ownership.

Heavily-changed shared infrastructure (regression risk for PA11-PA17):

- `dev/src/sema/decl_binder.cpp`, `decl_function.cpp`, `scope.h`,
  `scope_lookup.cpp` — PA11 dump path and lookup changes.
- `dev/src/sema/sem_call.cpp`, `sem_expr.cpp/.h`, `sem_operator.cpp`,
  `sem_convert.cpp` — overload-resolution changes (tie-breaks,
  qualification sub-rank).
- `dev/src/sema/sem_class.cpp`, `sem_member_body.cpp`,
  `sem_ctor.cpp`, `sem_lifetime.cpp` — class machinery reshuffle and
  deferred member bodies.
- `dev/src/lowering/lower_name.cpp` (+476) — template-aware Itanium
  mangling, local-name mangling, sanitized specialization scope paths.
- `dev/src/lowering/lower_unit.cpp`, `lower_function.cpp`,
  `lower_expr.cpp`, `lower_convert.cpp`, `lower_member.cpp` —
  fixture-derived lowering rules from `plan.md`.

### Performance risks to check

- Per-lookup or per-call linear scans over every captured template or
  every specialization (should be map/keyed access).
- Specialization cache keys recomputed in hot paths; repeated
  `DescribeType`/`TemplateArgumentKey` rendering per call site.
- End-of-TU `suspicious_names` recheck cost; pending-specialization
  worklist flush cost.
- Mangling substitution-table behavior on deep template types;
  repeated full-symbol-table walks in `lower_unit.cpp`.

### Ownership boundaries to check

- Template facts owned by `TemplateRegistry`/`TemplateInfo` in sema;
  lowering must consume typed sema facts, not re-derive them from
  entity-name strings (`Foo<int>` spellings are presentation only —
  verify nothing parses `<`/`>` back out of names for semantics).
- Instantiation driver must isolate all `SemBinder` mutable state
  (per plan.md); verify the save/restore set is complete and lives in
  one place.
- Fixture-derived lowering rules (per-element zero tails, narrowing
  folds, 8.5p7 zero-fill skip, empty-copy source evaluation, nullptr
  spelling) must be expressed as general type/semantic predicates in
  one owner, not scattered shape checks.

### Cheating / audit-integrity checks

- No test-name, fixture-name, or source-shape acceptance gates
  (grepped for test identifiers, `getenv`, content sniffing — initial
  sweep clean; verify per-file during review).
- No fallback success paths that emit dummy/empty LowIR instead of
  failing; no interpreter/VM/trampoline/embedded-payload substitutes —
  instantiation must produce ordinary sema entities lowered by the
  ordinary PA14-PA17 path.
- File-audit compliance: all sema/lowering files under the 1500-line
  cap without hidden fragments or code moved to unchecked paths
  (verify `frontend_source_sets.mk` change is a legitimate file
  addition).
- Definition-time sanity pass must reject only spec-anchored cases,
  not accept-list specific test inputs.

### Regression checks

- PA11 `BindTemplateDeclaration` dump behavior preserved
  (`BindMode::Types`).
- Monotonic extension: non-template programs must lower exactly as in
  PA17 (`make test-report-through-pa18` covers PA1-PA17 suites;
  confirm all stages green after changes).

## Findings

Four parallel deep reviews (template machinery; mangling/lower_unit;
fixture-derived lowering rules; shared sema infrastructure) plus direct
inspection. Consolidated, ranked by severity. Status marks:
[fixed] / [verified-ok] (inspected, judged correct as-is, rationale
given) / [noted].

### Blockers (fixed)

1. [fixed] `lowering/lower_expr.cpp` `BranchOnValue`: a branch on any
   namespace-scope pointer-to-function object spelled the object's
   *address* (no load) — generalized from one fixture
   (`100-function-template-explicit-specialization-address.ref`, whose
   reference output branches on `addr @global_passthrough`). The
   reference's shape is a fold of a provably-non-null condition (the
   global's only store is a function address in `__cppgm_init`); the
   unconditional replica miscompiled `void (*fp)() = 0; if (fp) ...`.
   Fix: the fold now requires proof — single unit, the global's
   registered dynamic init stores a function address, no other
   write/address-escape anywhere in the program, and no call in
   earlier init actions; otherwise a real load feeds the branch.
2. [fixed] `sema/template_deduce.cpp` `DeduceFunctionTemplate`: a
   catch-all around `EnsureFunctionSpecialization` turned failed
   substitution/instantiation into silent candidate dropping (de-facto
   SFINAE, explicitly out of scope) and left a poisoned cache entry
   (`body_emitted = true` was set before the body bind ran), so a
   later call could select a specialization whose definition was never
   emitted. Fix: substitution/instantiation errors are hard errors
   again; `body_emitted` is set only after a successful bind.
3. [fixed] `sema/sem_binder.h` `InstantiationContext` did not
   save/clear `param_capture_scope_` or `bf_units_written_`: a class
   instantiation triggered inside function-template signature
   composition leaked member parameter names into the outer capture
   scope (wrong trailing-return `decltype` resolution), and an
   instantiation inside constructor synthesis clobbered the bit-field
   first-write map. Both joined the RAII set.
4. [fixed] `sema/scope_lookup.cpp` unqualified directive walk: closure
   entries were searched via `QualifiedNamespaceSearch` at the outer
   anchor, re-anchoring transitively nominated names and hiding
   7.3.4p4 ambiguities (a transitive `A::x` could hide `::x` instead
   of being ambiguous beside it) — a latent regression for
   non-template programs using nested using-directives. Fix:
   transitive directives now join the closure with the *outer*
   directive's site (7.3.4p4 "as if ... appeared in the first"), and
   each nominated namespace is probed with a plain own-binding lookup.
5. [fixed] `sema/sem_operator.cpp` `AddTargetDeducedOverloads` pushed
   the same deduced specialization once per ranked candidate
   (`DeduceFunctionSetArguments` re-invokes it per candidate), making
   the function-set argument permanently non-viable ("exactly one
   match" rule saw duplicates). Fix: dedup on the specialization
   before appending.
6. [fixed] `sema/sem_convert.cpp` / `sem_call.cpp` 13.3.3.2p3: the
   qualification sub-rank was a binary flag, so two qualification
   conversions never compared (e.g. `f(const int*)` vs
   `f(const volatile int*)` on `int*` was wrongly ambiguous). Fix:
   conversions record the cv-union mask of their qualification steps;
   proper-subset masks rank per 13.3.3.2p3 bullet 2.
7. [fixed] `sema/template_info.cpp` `ArgSpelling` used unqualified
   class/enum names, so `Box<N1::A>` and `Box<N2::A>` produced
   identical entity/scope spellings — colliding printed symbol names
   and one `FunctionEntry` identity key (same-scope overloads over the
   two specializations merged). Fix: class/enum arguments spell their
   scope-qualified path. Companion fix: the specialization display
   name now qualifies by the template's declaring scope, not the
   first-use scope.
8. [fixed, re-scoped] the plan said "error at end-of-TU if still
   undefined and odr-used", and that rule was not implemented. During
   the fix the checked-in tests proved the plan text wrong: several
   compile-pass fixtures (e.g.
   `100-returned-class-template-prvalue-field-access`) call
   declared-only templates, and the references emit an ordinary
   external `declare function` for them (14.7.1: no diagnostic
   required). What was actually broken — and is now fixed — is the
   odr-use model itself: bodies instantiated eagerly for every deduced
   candidate (winners and losers alike, forcing the swallowing catch
   of finding 2). Specializations now record odr-use at the winner
   stamp points (a new `ISemExprHost::OnSpecializationOdrUsed` hook,
   including explicit template-id callees via `fn_self_spec` and
   explicit instantiations), bodies instantiate on first odr-use,
   pending instantiation at definition capture covers only odr-used
   specializations, and unevaluated operands (`decltype`, constant
   `sizeof`) correctly do not odr-use (3.2p2) — `declval`-style
   helpers stay uninstantiated. The plan text is corrected in the
   Architecture Review.
9. [fixed] `lowering/lower_name.cpp` `LowerScopeKey` embedded the
   `Scope*` in every named namespace/class segment, so identical named
   scopes from different translation units could no longer merge
   (multi-file inputs are in the assignment contract; `ns::f` declared
   in TU1 and defined in TU2 would split into a dangling declaration
   plus a renamed definition). Fix: named non-local scopes key by
   name; pointer tags remain only for unnamed scopes and
   function-local classes (the collision the tag was added for).
   Specialization scope names are unique after finding 7.
10. [fixed] `lowering/lower_name.cpp` `MangleLocalName` mangled the
    enclosing function via `FindOwnBinding(...)->type` — always the
    *first* overload, so local classes in a second overload mangled
    with the wrong signature and could collide. Fix: function scopes
    carry their own composed type (`Scope::fn_type`, stamped by the
    binder when the body scope is created); the mangler consumes the
    typed fact.
11. [verified, comment added] `lowering/lower_unit.cpp` C1/C2 pairing
    for instantiated constructors gated on `!cls->base` looked
    test-tuned (no comment, no apparent ABI basis). Empirically
    removing the condition failed exactly one witness
    (`100-inherited-class-template-conversion-operator`, a
    specialization *with* a base), which proves the reference's actual
    rule: with no bases the complete and base entries are one alias
    unit (identical work, one definition carrying both symbols), so
    demanding one emits both; with a base they are distinct
    definitions, each emitted on its own demand. The condition is the
    correct expression of that rule in the single-inheritance subset;
    it now carries the rationale and witness in a comment.
12. [fixed] `sema/template_deduce.cpp` `SameFunctionTemplateSignature`
    ignored the return type when neither declaration composed
    abstractly, merging distinct overloads
    (`typename T::A f(T)` / `typename T::B f(T)`) onto one record
    (spurious redefinition errors, wrong pattern instantiated). Fix:
    non-composing signatures also compare the declared return type
    structurally (AST type-id spelling).
13. [fixed] `sema/sem_template.cpp` `CheckMemberDefinitionAgainstPattern`
    paired an out-of-class special-member definition with a pattern
    declaration by parameter *count* only, misfiring on same-arity
    constructor overloads (spurious or missed noexcept-mismatch). Fix:
    pairing compares the declared parameter type-ids structurally,
    with arity as the fallback only when no structural match exists.

### Cleanups (fixed)

14. [fixed, residual warning explained] `sema/sem_binder.h` gained a
    new file-audit warning (`bad-division: header contains
    substantial implementation body`) in the PA18 window. The genuine
    implementation fragment — the `InstantiationContext`
    constructor/destructor bodies — moved out-of-line to
    `sem_template.cpp` (where the save/restore set is also easier to
    keep complete). The warning itself persists because the
    heuristic counts every `{`/`}`/`;` line, i.e. it is a
    declaration-count proxy: SemBinder's ~450 method-declaration
    lines trip the 180-line threshold with zero implementation in
    the header. It is non-fatal, the same class as the pre-existing
    `parser.h` warning tolerated since earlier stages, and hides no
    code; restructuring SemBinder into several classes to silence a
    proxy metric would be churn, not cleanup.
15. [fixed] `sema/scope.cpp` `SetMemberScope` stored the scope→entity
    fact twice (`Scope::entity` and `TypesModel::scope_entities_`),
    two owners free to drift. The map was removed; `ScopeEntity()`
    reads the scope field.
16. [fixed] `sema/template_deduce.cpp`: instantiation-depth literal
    `200` duplicated `kMaxInstantiationDepth`; the ~25-line parameter
    pre-bind loop was duplicated between `EnsureFunctionSpecialization`
    and `InstantiateFunctionBody`. Shared constant and helper factored.
17. [verified by counterexample] `sema/template_deduce.cpp`
    `ResolveFunctionTemplateId` swallowing argument-list errors was
    flagged as degraded diagnostics. Implementing the "propagate for a
    sole template" version broke partial-explicit template-id calls
    (`fusion::remove<X>(value)`): an argument list that binds only
    some parameters is the *designed* 14.8.1 signal to fall back to
    the overload set so the call context deduces the rest. The
    swallow-and-continue is principled; it now carries a comment
    saying so. The eager-body-instantiation half of the finding was
    mooted by finding 8 (resolution now composes signatures only).

18. [fixed] `lower_member.cpp` empty-object copy: the member-addressed
    path skipped the source lvalue entirely, dropping side effects —
    confirmed by direct experiment (`struct E{}; struct W { E e;
    W(E* (*get)()) : e(*get()) {} };` compiled to a constructor that
    never calls `get`). Fix: the source elides only when it is
    effect-free (`ExprHasSideEffects` over the sem tree); a
    side-effecting source still evaluates, and only the `copyobj` is
    skipped. The "empty" test itself was already principled
    (`class_record->is_empty` from layout).

### Verified as correct (no change)
- `instantiating_`-gated defaulted-copy synthesis
  (`sem_class.cpp` → `EnsureSpecialCtor`) and `weak_def` stamping:
  the witness set *discriminates* between the two candidate rules —
  `RefPair<int>` (a specialization) whose defaulted copy is odr-used
  only from `main` emits no definition, while `Box<int>`'s copy
  odr-used inside an instantiated body does — so the mode-based gate
  (synthesis happens during instantiation) is the rule the reference
  compiler observably implements; a class-identity gate is refuted by
  the witnesses.
- Fixture-derived rules verified general: per-element zero tails (keyed
  on element type, floats keep `zeroinit`), narrowing immediate-fold
  (pure width/signedness property), 8.5p7 zero-fill skip (standard's
  user-provided definition), TLS guarded init (storage/type property),
  braced-init-list returns (loud subset failure on scalar forms),
  nullptr spelling (semantic-node property, uniform across types).
- Definition-time sanity pass (`sem_template_check.cpp`): conservative
  and spec-anchored — shadow check is general 14.6.1p6, unresolved
  names re-check at end of TU from the declaring scope, dependent
  bases/using-declarations/ADL-callees disable the check. No
  test-shape gates.
- No test-name/fixture-name/env gates, no content sniffing, no
  timeout workarounds, no fallback-success paths (unhandled forms
  throw `OutsideBoundary`/`OutsideSubset`; no swallowing catch in
  lowering), no interpreter/VM/trampoline/embedded-payload
  substitutes anywhere in the reviewed surface. Harness, comparator
  scripts, Makefiles, and checked-in tests/refs untouched in the PA18
  window (`git diff db7df0650..HEAD` over those paths is empty).
- File audit: all files under the cap; splits
  (`sem_call.cpp`, `sem_member_body.cpp`, `decl_function.cpp`) are
  cohesive per-topic method files, no mutable globals or hidden
  coupling; `frontend_source_sets.mk` change only adds the new
  sources to the build.

### Noted (accepted, with rationale)

- `MemberDefinitionKey` (`lower_unit.cpp`) keys ctor/dtor definitions
  without the spelled name because pattern-spelled definitions and
  specialization-spelled call sites must pair; identity rests on the
  class scope pointer + kind + type, which is sound. The dual
  spelling of `entity_name` is a presentation-layer convention, not a
  semantic channel.
- Mangler `Substitutions::Find` is a linear scan and component keys
  are re-rendered per enclosing level: per-symbol, small-table costs
  (standard for Itanium manglers); the demand machinery itself is
  O(1) per flip with no repeated whole-table walks. Explicit
  instantiation rescans `unit.deferred` per directive — bounded by
  the (small) number of explicit instantiations.
- Per-call constant-factor allocations added to overload resolution
  (empty template vectors, an order-tracking set) — the letter of the
  monotonic-extension rule holds (no template work runs for
  template-free programs); constant factors only.
- `ScopeBinding` parallel per-overload vectors (`fn_defaults`,
  `fn_deleted`, `fn_owner`, `fn_param_names`, `fn_templates`) follow
  the codebase's established parallel-array style; all reads are
  slot-guarded.

## Changes Made

Two cohesive commits on top of `9daaaa4ee`:

1. **Sema round** — odr-use-driven instantiation, hard substitution
   errors, lookup and conversion fixes:
   - `sema/sem_expr.h`: new `ISemExprHost::OnSpecializationOdrUsed`
     and `SwapUnevaluatedOperand` hooks.
   - `sema/template_deduce.cpp`: `EnsureFunctionSpecialization` is
     substitution-only; `InstantiateFunctionBody` runs on odr-use;
     substitution failures propagate (swallowing catches removed from
     `DeduceFunctionTemplate` / `DeduceFunctionTemplateFromTarget`);
     `InstantiatePendingFunctions` covers only odr-used specs;
     explicit instantiation marks odr-use; shared
     `kTemplateInstantiationDepthLimit`; parameter pre-bind loop
     factored into `PreBindDeclaredParameters`; non-composing
     signature identity also compares the positional return spelling.
   - `sema/sem_template.cpp`: `InstantiationContext` bodies
     out-of-line, absorbing `instantiating_`, `param_capture_scope_`,
     `bf_units_written_`, and `in_unevaluated_operand_`;
     specialization displays qualify by the declaring scope;
     structural member-definition pairing.
   - `sema/template_info.h/.cpp`: `odr_used` flag; qualified
     argument spellings (`EntityScopePrefix`); positional
     canonical-spelling helpers.
   - `sema/scope_lookup.cpp`: 7.3.4p4 site-propagating directive
     closure; plain own-binding probe per nominated namespace.
   - `sema/sem_convert.h/.cpp`, `sema/type.h/.cpp`: 13.3.3.2p3
     cv-signature proper-subset rank (`qual_dest`,
     `CvSignatureProperSubset`).
   - `sema/sem_operator.cpp`: target-deduced overload dedup.
   - `sema/sem_call.cpp`, `sem_expr.cpp`, `sem_operator.cpp`: odr-use
     stamps at the five winner points (incl. `fn_self_spec` callees).
   - `sema/sem_binder.cpp`, `sem_cast.cpp`: unevaluated-operand
     marking for `decltype` and `sizeof(expr)` operands.
   - `sema/scope.h/.cpp`: `Scope::entity` is the single owner of the
     scope→entity fact (map removed).

2. **Lowering round** — provable branch fold, scope keys, local
   mangling, empty-copy side effects:
   - `lowering/lower_expr.cpp` + `lower_unit.cpp` +
     `lower_program.h`: the namespace-scope function-pointer branch
     spells the object's address only under proof
     (`BranchSpellsFnPointerAddress`: single unit, dynamic init
     stores a named entity's address, no write/alias anywhere —
     conservative `TreeHasUnsafeUse` over the whole program — and no
     call runs before the init store); otherwise the value loads.
   - `lowering/lower_name.cpp`: `LowerScopeKey` keys globally-named
     scopes by name (cross-unit merge restored) and pointer-tags
     unnamed scopes, function-local classes, and specializations
     whose argument chain involves local/unnamed entities
     (`ArgSpellingIsGlobal` over typed `spec_args`);
     `MangleLocalName` consumes the new `Scope::fn_type` fact instead
     of first-overload name lookup.
   - `sema/scope.h`, `decl_function.cpp`, `sem_class.cpp`,
     `template_deduce.cpp`: function body scopes carry their composed
     type (`Scope::fn_type`).
   - `lowering/lower_unit.cpp`: C1/C2 pairing comment records the
     alias-unit rule and its discriminating witness.
   - `lowering/lower_member.cpp`: member-addressed empty-copy
     evaluates side-effecting sources (`ExprHasSideEffects`).

## Validation

- `make test-report-through-pa18`: **1367 / 1367** (all 18 stages,
  193/193 pa18) after each round; re-run green at the end.
- `perl scripts/cppgm_file_audit.pl --stage pa18 --paths dev/src`:
  **pass** (2 non-fatal warnings: the pre-existing `parser.h` one and
  the `sem_binder.h` declaration-count proxy explained in finding 14).
- Directed adversarial checks (beyond the suite):
  - `fp = 0; if (fp)` and `fp = &g; set(){fp=0;}; if (fp)` both emit
    `load` + `branch` (miscompile of finding 1 gone); the pinned
    fixture still folds under proof and matches its reference.
  - The finding-18 reproducer now calls `get()` inside `W::W`.
  - Removing the `!cls->base` pairing condition fails exactly the
    with-base witness (finding 11's verification).
  - The finding-8 restructure was driven green against
    `declval`-style unevaluated uses and declared-only odr-used
    templates from the checked-in suite.
- Cross-cutting sweeps re-confirmed at the end: no test-name/env/shape
  gates, no harness/comparator/test changes in the PA18 window, no
  swallowing catch left on the instantiation paths.
