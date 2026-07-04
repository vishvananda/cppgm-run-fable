# PA22 Plan: Deduction, Substitution, and SFINAE Completion

## Goal

Finish the deduction/substitution half of template completion on top of the
PA18-PA21 machinery: full function-template call deduction over the C++11
subset, function-template partial ordering, SFINAE candidate dropping,
conversion/constructor template participation, non-deduced contexts,
no-eager-instantiation timing, and pointer/reference/enum/static-member
non-type template arguments. Output stays LowIR through the existing
PA14-PA21 lowering path.

## Baseline

- 95/176 pa22 tests pass before this work; all 81 failures are pa22-local
  (through-pa21 remains green at 1731 tests).
- The PA18-PA21 model already carries: structural deduction
  (`template_deduce.cpp`), explicit-argument pre-binding, forwarding
  references, trailing-pack absorption, partial-specialization ordering,
  member/alias/friend templates, and default template-argument fill during
  deduction.

## Design

### Ownership boundaries

- `template_deduce.cpp` keeps unification and candidate formation;
  substitution failure becomes candidate state there, not a diagnostic.
- `template_args.cpp` keeps argument resolution; it learns
  address-valued (pointer/reference/static-member) non-type arguments.
- `sem_call.cpp` / `sem_operator.cpp` / `sem_member.cpp` keep overload
  resolution; they only see fewer/more candidates, plus function-template
  partial-ordering tie-breaks that already route through
  `TemplateCandidateMoreSpecialized`.
- `sem_template.cpp` owns instantiation timing: naming a class-template
  specialization creates its record; only completeness demand instantiates.
- Lowering keeps demand-driven emission; declaration-only function-template
  specializations mangle through the same
  `MangleFunctionTemplateObjectName` path as defined ones and stay weak.

### SFINAE core (task 1)

- `EnsureFunctionSpecialization` failures while composing the concrete
  signature (parameter/return substitution, default template arguments,
  trailing decltype analysis, enable_if member lookup) are immediate-context
  failures: `DeduceFunctionTemplate` catches them and returns no candidate.
- Hard-error boundary: errors raised while instantiating a class *body*
  (completeness demand inside substitution) stay hard; the catch wraps only
  the signature-composition step, and class-body instantiation faults are
  re-thrown through a non-SFINAE exception type when they occur below a
  substitution probe.
- The specialization cache stays clean on failure (records enter the cache
  only after composition succeeds) so a later identical call re-probes.

### Expression SFINAE (task 2)

- Trailing-return `decltype(expr)` and default-argument probes analyze the
  substituted expression through the ordinary analyzer inside the
  composition step, so member-lookup/call/conversion failures become
  deduction failures via task 1.
- Class-scope unevaluated calls (`decltype(check<F>(0))`, `sizeof(f(0))`)
  resolve static member templates without an object: the static-method call
  path learns template-id callees and member-template candidates.
- `sizeof(expr)` joins the constant-expression subset as size-of the
  analyzed operand type (unevaluated operand).

### Deduction completion (task 3)

- Explicit template arguments substitute into parameter patterns before
  deduction: a pattern whose mentioned parameters are all bound becomes a
  concrete parameter checked by conversion, not unification (14.8.2.1p4's
  participation rule). This covers explicit-id calls needing user
  conversions and same-name shadowing cases.
- Overload-set arguments (14.8.2.1p6 / 14.8.2.2): deduction tries each
  member of the set; exactly one success binds, more than one makes the
  parameter non-deduced (fixes the bad-accept).
- Derived-to-base template-spec deduction walks all bases (not just the
  single chain) and requires a unique match.
- Conversion-function templates (14.8.2.3) deduce against the target type
  and participate in user-conversion selection.
- Constructor templates keep their PA21 participation; braced-init
  deduction covers the supported call subset.

### Partial ordering completion (task 4)

- 14.8.2.4p9 tie-breaks land: when both patterns deduce, reference vs
  non-reference and more-cv-qualified references order (`T&` beats
  `const T&` for non-const lvalues; array/cv reference cases).
- Ordering records the reference/cv adjustment per parameter position
  during the transformed-type walk instead of throwing the information
  away before deduction.

### Function-template identity (task 5)

- Two function-template declarations are the same template only when their
  template parameter lists match positionally in kind *and* in the
  positionalized spelling of each non-type parameter's type (14.5.6.1);
  enable_if-defaulted NTTPs with different condition spellings declare
  distinct overloads instead of colliding as redefinitions.

### No-eager-instantiation (task 6)

- Naming `X<args>` in a typedef/alias/template-argument creates the
  specialization record without instantiating members or requiring the
  pattern to substitute; completeness demand (member access, base class,
  object creation, sizeof) triggers instantiation as today.
- Instantiation faults therefore surface at the completeness-demand site,
  never at the naming site.

### Non-type template arguments over entities (task 7)

- `TemplateArg` value form learns an entity-address payload (variable,
  static data member, function) with cv-stripped identity; keys, spelling,
  equality, and dependence learn the form.
- Parameter types `T&`, `T*`, and enum compose and check their arguments
  per 14.3.2 (no temporaries, external linkage per the C++11 subset).
- Lowering spells entity-address arguments through the existing symbol
  model (address-of-global initializers already exist from PA16).

### Linkage/mangling of declaration-only specializations (task 8)

- A deduced specialization of a template with no definition lowers as a
  `declare function` with weak binding and the standard
  `<name>I<args>E` mangling (already used for defined specializations);
  the plain-identifier fallback that embedded template-id text in the
  mangled name is removed.

## Status (2026-07-04)

IN PROGRESS: 155/176 pa22 tests pass (95 at baseline); through-pa21
stays fully green (1731/1731) after every landed batch.

Landed: SFINAE candidate dropping (tasks 1-2), template-head identity
with out-of-class pairing tolerance, template-id overload sets,
no-eager-instantiation with demand sites (new-expressions, class
copies, returns, conversion classification via binder hooks, explicit
instantiation, decltype prefixes), the retry-loop use-after-free and
dependent-name mangling fixes, declaration-only spec mangling/weak
linkage, overload-set argument deduction (14.8.2.1p6), explicit-arg
conversion fallback, partial-ordering ref/cv tie-breaks with unique
array bounds, conversion-function templates end to end (capture,
destination deduction, synthesized entries, bodies, cv mangling),
constructor templates in implicit conversions, inherited constructor
templates (using Base<T>::Base), entity-valued NTTPs
(reference/pointer forms with L_Z mangling), dependent alias-use
re-substitution in partial-spec matching (void_t detectors),
pack-slot deduction through template-id patterns, ellipsis-riding
trailing arguments, explicit `Args...` splicing, surrogate calls
(13.3.1.1.2), decltype base-specifiers, and pack expansions in
paren-init declarations.

Also landed since: multi-pack slot deduction/keys, CWG1558 void_t
deferral with nested tt-application re-substitution (detected_or),
hidden-friend ADL through using-declaration-owned bindings, friend
outer-argument aliasing, 14.1p10 default accumulation, null-pointer
NTTP defaults, non-type declarator packs, zero-arg constexpr-call
value arguments, surrogate calls, abstract-array SFINAE, decltype
base-specifiers and prefix demands, one-byte pointer-difference fold.

Remaining (21): two-phase-lookup timing for later-declared shadowing
values (decl-order-aware lookup), value-pack static-data members with
sizeof... bounds, a few deep alias/pack SFINAE compositions
(and_helper chains inside member guards), several exact-LowIR shape
diffs (empty-temporary ctor elision in delegating mem-initializers,
canonical function ordering), and assorted single-feature corners;
each needs its own reduction.

## Validation

- Fast loop: `make test-report ACTIVE_TEST_REPORT_PAS='pa22'` and
  `make -C pa22 check TEST=tests/...` for single cases.
- Gate: root `make test-report-through-pa22` after each task lands
  (template machinery is shared from PA18 on; earlier suites must stay
  green).
- File audit: `perl scripts/cppgm_file_audit.pl --stage pa22 --paths
  dev/src`; new SFINAE/deduction code stays in the existing deduction and
  argument-resolution units unless size forces a split.
- The three segfaulting inputs (default-argument prefix deduction through
  operator templates) get fixed before feature batches continue, since a
  crash can hide any number of later failures.
