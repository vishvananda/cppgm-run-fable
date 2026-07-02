# PA18 Plan: First-Tier Templates (`cppgm++ --emit-lowir`)

## Goal

Add class templates and function templates with type parameters, default
type arguments, direct-call deduction, dependent names, and on-demand
instantiation on top of the PA17 compiler, emitting LowIR through the
existing PA14-PA17 lowering path. No new backend work: instantiation
produces ordinary sema entities that lower through existing machinery.

## Architecture Recap (what we build on)

- `AstParser` (PA10) already parses all template syntax into `AstDecl`
  (`DK_TEMPLATE` with `template_params` + `inner`,
  `DK_EXPLICIT_INSTANTIATION`) and `AstName` parts (`NP_TEMPLATE_ID`
  with `AstTemplateArgument`s). The AST for a translation unit stays
  alive for the whole bind, and `SemBinder` already re-walks stored AST
  (`DeferredBody`) for member bodies.
- `DeclBinder::BindTemplateDeclaration` (PA11) binds parameters as
  `TK_TYPE_PARAM` named types in a `SCOPE_TEMPLATE_PARAMS` scope and
  binds the inner declaration for the types dump. That behavior must
  stay for the PA11 dump path (`BindMode::Types`).
- `SemBinder` binds one TU in a single forward pass; function bodies
  produce `SemNode` trees into `SemUnit.items` (strong) or
  `SemUnit.deferred` (weak, demand-emitted by `LowerProgram`).
- Lowering demand-emits weak definitions (`RegisterDeferred` /
  `DemandFunction`), mangles with the existing Itanium mangler.
- The pa18 LowIR comparison canonicalizes symbol names and drops
  `object=`/`binding=` metadata; `object=` manglings are used as
  pairing hints, so template manglings should be Itanium-correct where
  practical (pattern types with `T_`/`T0_` references + `I..E`
  argument lists + return type), but exact parity is not the gate.

## Design

### Ownership boundaries

New files (respecting the 1500-line audit cap; all sema-owned):

- `dev/src/sema/template_info.h` — data model: `TemplateInfo` (one
  template declaration: kind class/function, `const AstDecl*` (the
  `DK_TEMPLATE` node), parameter list view, lexical `Scope*`, name,
  the class-template member-definition attachments); specialization
  records (`ClassSpecialization`: canonical args key ->
  `NamedTypeInfo*` + state; `FunctionSpecialization`: args key ->
  signature + emitted flag); `TemplateRegistry` owning all
  `TemplateInfo`s for a TU.
- `dev/src/sema/sem_template.cpp` — SemBinder template machinery:
  capture of template declarations, class-template instantiation
  driver, out-of-class template member definition registration and
  instantiation, explicit instantiation, static-member instantiation.
- `dev/src/sema/template_deduce.cpp` — function-template argument
  deduction (14.8.2.1 subset) + function-template candidate
  preparation for overload resolution.
- Small extensions to existing files: `scope.h` (template bindings on
  `ScopeBinding`), `type_builder.cpp`/`decl_parser.cpp` (template-id
  type resolution hook), `sem_expr.cpp` (template candidates in
  calls), `lower_name.cpp`/`abi_mangle` (template-id mangling),
  `sem_class.cpp` (instantiation re-entrancy seams).

The `TemplateRegistry` lives in `SemBinder` (per TU, like the model);
AST lifetime already spans the whole bind, so `const AstDecl*` capture
is safe.

### Name binding of templates

- Class template `template<class T> struct Foo {...};` binds `Foo` in
  the declaring scope as a new binding kind `SB_CLASS_TEMPLATE`
  carrying `TemplateInfo*`. Redeclarations (forward declaration then
  definition, possibly with renamed parameters) merge onto one
  `TemplateInfo`; the definition's AST wins as the instantiation
  pattern (parameter identity is positional).
- Function template binds into the ordinary `SB_FUNCTION` binding
  under its name with a parallel `fn_templates` vector on
  `ScopeBinding` (so template and non-template overloads share one
  name, and using-declarations/ADL carry them together). A
  function-template-only name still creates an `SB_FUNCTION` binding
  with zero ordinary overloads.
- Member templates are out of scope; a template declaration directly
  inside a class body is a boundary error except the supported
  out-of-class definition forms.

### Semantic analysis policy (single-phase at instantiation)

Template *bodies are not analyzed at declaration*. Full two-phase
lookup is explicitly out of scope; instantiation happens at the point
of use inside the single forward pass, so names declared later in the
TU are naturally invisible — a good approximation of
point-of-instantiation semantics that the tests exercise
(`300-dependent-adl-point-of-instantiation`).

Exception (per test `100-nondependent-template-member-body-lookup-bad`
and `100-local-alias-shadows-template-parameter-bad`): a cheap
definition-time sanity pass over template bodies is required to reject
clearly-ill-formed non-dependent constructs. Implemented as a
lightweight AST walk that checks (a) unqualified names used in
expressions resolve *somewhere* (current scope chain or a dependent
context: any expression involving template parameters, members of the
current instantiation, or dependent bases disables the check for that
name), and (b) no local declaration redeclares a template-parameter
name (14.6.1p6). This walk must stay conservative: silence is always
allowed except for the specific spec-anchored rejections in the
tests.

### Class template instantiation

Entry point: type resolution meets `Foo<args>` (NP_TEMPLATE_ID) in a
type position, `Foo<args>::` qualifier, or explicit instantiation.

1. Resolve `Foo` by ordinary lookup to `SB_CLASS_TEMPLATE`.
2. Resolve each template argument type-id in the *use* context.
3. Apply default arguments: re-resolve default type-ids in the
   template's declaring scope under a parameter scope where earlier
   parameters are bound (aliased) to the already-resolved arguments.
4. Canonical key = vector of resolved `TypePtr`s (rendered through
   `DescribeType` over canonicalized types; identity via structural
   equality). Cache hit -> existing `NamedTypeInfo*` (possibly still
   open: current-instantiation self-reference like `Foo<T>` inside the
   body resolves to the in-progress entity, legal wherever an
   incomplete class type is legal).
5. Cache miss -> instantiate: save full binder context (current
   scope, open classes, deferred bodies list, method context, dump
   parents, access, return type), switch to the template's lexical
   scope, push a fresh `SCOPE_TEMPLATE_PARAMS` scope binding each
   parameter name as `SB_TYPE_ALIAS` to its concrete argument, then
   run the ordinary class binding path over the stored class AST
   (`BindDeclaration` on the inner `DK_CLASS`), so PA15-PA17 class
   machinery (layout, specials, vtables, member bodies via
   `DeferredBody`) runs untouched. Restore context afterwards.
   The instantiated entity's `NamedTypeInfo.name` is the canonical
   `Foo<int>` spelling; its member scope, `ClassInfo`, bases, etc. are
   ordinary.
6. Member function bodies + static member definitions attached to the
   template (in-class bodies now; out-of-class definitions registered
   on the `TemplateInfo` as they are parsed) are *not* all bound
   eagerly: in-class bodies flow through the existing deferred-body
   flush at instantiation completion (they are ordinary weak inline
   definitions); out-of-class member definitions instantiate on
   demand — first odr-use of that member for that specialization — or
   at explicit instantiation. If the out-of-class definition appears
   *after* the class was instantiated, uses after that point can still
   demand it (registry records pending member definitions per
   specialization).
7. Instantiated member definitions and static data members emit as
   weak (`SemUnit.deferred`), matching `binding=weak` in the refs.
   Explicit instantiation (`template struct Foo<int>;` /
   `template class ...`) forces instantiation of all member
   definitions available at that point (strong emission per 14.7.2 is
   relaxed to the checked refs' expectations — verify with the
   explicit-instantiation tests).

Re-entrancy: instantiation can trigger inside expression analysis,
inside another instantiation, or inside an open class. The
instantiation driver isolates *all* SemBinder mutable state and the
deferred-body queue; instantiated dump nodes append to
`SemUnit.deferred` (never the open `parents_` chain). A
per-registry "in progress" set detects cyclic instantiation
(self-referential layout) and reports an error.

### Function template deduction and instantiation

At call analysis, when a callee name's lookup (ordinary or ADL) or an
operator candidate sweep finds `fn_templates`:

1. For each function template: bind explicit template arguments from
   an `NP_TEMPLATE_ID` callee if present (positional); remaining
   parameters deduce.
2. Deduction (14.8.2.1 subset): unify each function parameter pattern
   against the argument type. Pattern composition binds parameters as
   abstract `TK_TYPE_PARAM` types (the PA11 machinery) — done lazily
   per template, cached on the `TemplateInfo`. Parameter patterns that
   cannot compose abstractly (dependent qualified types like
   `typename T::x`) are non-deduced contexts: skipped during
   unification, substituted after the other parameters deduce.
   Unification handles: exact `T`, cv `T`, `T*`, `T&`, `T&&`,
   arrays, pointers-to-member, function pointers, and class template
   specializations `Foo<T>` (match a specialization of the same
   template and recurse into its argument list); top-level cv and
   array/function-to-pointer decay per 14.8.2.1p2.
3. Success -> substitute: compose the full signature concretely with
   parameters aliased to the deduced types (through the ordinary type
   builder in the template's lexical context, so `typename T::x`
   resolves for real). Substitution failure is a hard error (SFINAE
   out of scope).
4. The deduced specialization joins the candidate set. Overload
   resolution runs unchanged, except the 13.3.3 tie-breaker: prefer a
   non-template candidate over a template specialization when
   conversion sequences tie. (Partial ordering between two templates
   is out of scope; two equally good template candidates are
   ambiguous.)
5. If the template candidate wins: instantiate the body on demand
   (once per canonical argument key): bind the stored function
   definition AST in the template's lexical scope + concrete parameter
   alias scope, emitting a weak `SN_FUNCTION_DEFINITION` into
   `SemUnit.deferred` under its specialization identity. Declarations
   without a visible definition at the first use: record the pending
   call; a later definition in the TU must instantiate then (tests
   with forward-declared templates defined later). Simplest correct
   order: keep a worklist of needed-but-undefined specializations,
   flushed when the template's definition is captured and at
   end-of-TU (error there if still undefined and odr-used).
6. Function address contexts (`&f` with target type;
   `100-overloaded-function-address-context`) — deduce against the
   target function type for the template members of the overload set.
7. Conversion-operator templates and operator templates participate
   through the existing operator/conversion candidate collection;
   `fn_templates` ride along on the bindings those sweeps visit.

### Dependent names inside supported template bodies

Because bodies are only analyzed at instantiation with all parameters
aliased to concrete types, dependent lookup mostly reduces to ordinary
lookup. Specific support still needed:

- `typename T::type` in type positions: the type builder's qualified
  name resolution already walks class member scopes once `T` aliases
  to a class; accept and ignore a leading `typename` in the supported
  positions (parse side already carries it); *require* it for
  dependent qualified types in the one `-bad` test (definition-time
  check: qualified type-position name whose qualifier is a template
  parameter without `typename` -> error at definition time).
- `x.template get<int>()` / `T::template ...`: parse-side
  `template_keyword` flag already exists; sema treats it as a plain
  member/qualified template-id.
- Current instantiation: `Foo<T>` (and plain injected `Foo` where
  PA15 already injects the class name) inside the body resolves via
  the cache to the in-progress specialization.
- Dependent bases: base-clause resolution happens at instantiation
  (concrete), so `this->member` and qualified base member access are
  ordinary PA15-17 lookups. Unqualified names in template bodies do
  NOT search dependent bases per 14.6.2p3
  (`300-unqualified-call-skips-dependent-base`): during instantiation
  body analysis of a class whose *pattern* had a dependent base, an
  unqualified lookup that would resolve into that base scope must
  skip it. Record on the instantiated `ClassInfo` that the base was
  dependent in the pattern; member-scope lookup consults that flag for
  unqualified (non-`this->`, non-qualified) name resolution.

### Mangling and LowIR identity

- Instantiated functions/methods get LowIR names derived from the
  source name (`@f`, uniquified by the existing overload-index
  machinery) and `object=` Itanium manglings extended with:
  `I <type>+ E` template-argument lists, template-parameter
  back-references `T_`/`Tn_` in the *pattern* signature, return type
  included for function templates, and substitution entries per the
  Itanium rules the existing mangler already implements for ordinary
  types. Class specializations mangle as `3FooIiE` wherever class
  names mangle today.
- The comparator drops `object=` from the diff but uses it to pair
  functions; shape-pairing covers most cases, so mangling
  correctness is "best effort, structurally valid, unique".

### Witness output

`.ref.witness` files are only compared by `test-strict`
(`compare_witness_results.pl`), which is not part of the
`test-report-through-pa18` gate. Witness emission is deferred until a
stage requires it.

## Validation plan

- Fast loop: `make test-report ACTIVE_TEST_REPORT_PAS='pa18'`;
  single test: `make -C pa18 check TEST=tests/spec/....t`.
- Gate after each meaningful shared-infra change:
  `make test-report-through-pa18` (older stages must stay green —
  particularly PA11 dump behavior around `BindTemplateDeclaration`
  and PA12-17 overload resolution, which template candidates touch).
- `perl scripts/cppgm_file_audit.pl --stage pa18 --paths dev/src`
  after the new files land.
- Order of implementation: capture + class instantiation for the
  simplest spec tests first, then function deduction, then the
  out-of-class/static/nested member forms, then dependent-name edge
  tests, then the 4 compile-fail tests.

## Status

- [x] Architecture mapped; comparison/canonicalization rules read.
- [x] Template capture (class + function) without body analysis.
- [x] Class template instantiation on demand + cache (re-entrant
      context save/restore; injected-class-name re-enters the
      template, 14.6.1p1).
- [x] Default template arguments (including references to earlier
      parameters).
- [x] Function template deduction for direct calls, explicit and
      partial-explicit template-ids (14.8.1), target-directed
      deduction (13.4p2), forwarding references, derived-to-base
      `Foo<T>` matching.
- [x] Non-template-preferred tie-break; template specializations in
      overload sets and operator candidate sweeps.
- [x] Out-of-class member function/static member/nested-class member
      definitions (registered on TemplateInfo; instantiated per
      specialization); member classes defer per 14.7.1p1 and complete
      on demand (fields, variables, member access, sizeof, qualified
      lookup, constant-expression layout).
- [x] Explicit instantiation (class + function forms) with
      `object_root=yes` member emission (14.7.2p8).
- [x] Operator templates via ADL/using declarations; ADL associates
      specialization argument namespaces (3.4.2p2).
- [x] Dependent-base unqualified-lookup skip (14.6.2p3); `typename`
      requirement (14.6p3); definition-time sanity pass
      (parameter-shadow, unresolved non-dependent names, out-of-class
      noexcept mismatch).
- [x] Template-aware mangling (`I..E`, `T_`/`Tn_` back-references,
      template-name substitution candidates, return type for function
      templates); sanitized specialization scope paths; weak
      instantiated definitions and static members.
- [x] Trailing-return decltype over the parameters (8.3.5p2 ordering)
      during substitution and body instantiation.
- [x] Full pa18 suite green through `make test-report-through-pa18`:
      1367/1367 (193/193 pa18), file audit clean. Final round added:
      partial-ordering tie-break (14.5.6.2 subset over the deduction
      patterns), qualification-conversion sub-rank (13.3.3.2p3),
      xvalue reference-member binding, member-signature `this`
      context (5.1.1p3 trailing-return decltype), out-of-class member
      instantiation through pattern names/class-scope declarator
      lookup, TLS first-use guarded init (`__tls_guard` +
      `__tls_init`), Itanium local-name mangling (`Z<fn>E<name>`),
      braced-init-list returns, derived-to-base reference casts,
      poisoned instantiated member bodies (ill-formed only when
      demanded, 14.7.1), `using Alias::Alias` constructor
      inheritance, `decltype(x)::member` qualifiers, `sizeof(expr)`
      over unevaluated operands, variable-template parse-and-ignore,
      inline-namespace directive lookup, and several fixture-pinned
      lowering shapes (per-element aggregate zero tails, folded
      narrowing immediate conversions, 8.5p7 zero-fill skip,
      empty-copy source-evaluation rules, instantiated ctor C1/C2
      pairing for baseless specializations).

### Fixture-derived lowering rules (reference parity notes)

- Local scalar-array aggregate tails: the reference stores each
  value-initialized tail element individually (`store i32 0` per
  element), not one `zeroinit` span; keep `zeroinit` only for
  floating tails (no fixture pins those) and dynamic array-new fill.
- A user-`= default` trivial copy/move constructor odr-used *inside
  an instantiated template body* still synthesizes and emits its
  weak definition (14.7.1-adjacent; witness: the Box test emits
  `_ZN3BoxIiEC1ERKS0_` with `trivial_lifecycle=yes`, while RefPair,
  whose defaulted copy is used only from `main`, emits none, and the
  ADL test's *implicit* trivial copy inside `call_pick` emits none).
  Call sites still lower as raw `copyobj`; only the demanded
  definition prints. Ownership: sema builds the body on selection
  (`MakeConstructorCall` trivial path → `EnsureSpecialCtor` when
  `instantiating_ && selected.defaulted`); the lowering demands the
  registered synthesized body from the trivial-copy action.
- Empty-object copies: a member-addressed trivial-copy action prints
  only the member (destination) address; an argument/temporary copy
  still evaluates the source lvalue and its base-adjust hop, and only
  the `copyobj` is skipped.
- Branching on a namespace-scope pointer-to-function object spells
  the object's address (fixture-pinned reference presentation).
- Value-initialized pointer prvalues spell the immediate `nullptr`;
  retyped integer zeros keep the immediate `0`; the `nullptr` keyword
  materializes through `copy ptr nullptr`.
