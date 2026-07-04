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

### Dependent-signature Itanium mangling (task 9)

- The `object=` name of a function-template specialization must spell
  the template's as-written signature (5.1.5.2): `T&&` is `OT_`,
  `typename remove_reference<T>::type&` is
  `RN16remove_referenceIS3_E4typeE`, a pack parameter is
  `Dp<element>`, and a written `T...` inside a dependent template-id is
  `JDpT0_E`. Today the mangler substitutes the concrete signature
  whenever the abstract pattern is null (dependent forms) or has pack
  parameters, so those specializations fail to pair with the reference
  and the compare falls back to emission order (first blocker:
  `100-type-pack-element-preserves-concrete-argument`).
- Ownership: mangling of dependent forms is syntax-directed in the ABI,
  so the mangler walks the written pattern (`tmpl.pattern_decl`) for
  exactly the pieces that do not compose into `TypePtr` patterns;
  composable pieces keep mangling from the typed pattern
  (typedef-resolved, adjusted). `EnsureFunctionPattern` additionally
  composes a `return_pattern` for signatures whose full pattern fails
  (prefix `*`/`&`/`&&`/cv declarator items only), so a composable
  return like `T&&` still mangles from typed state.
- Substitution-table alignment: written-form components and typed
  components must share substitution keys (`TP:n` for parameter
  references, `T:name<argkeys>` for template-ids, `...` marks pack
  patterns) so a written return type and a typed parameter compress
  against each other exactly like the reference
  (`RS7_` in `get`'s mangling).
- Value-parameter references in written template-ids spell `XT_E`
  (not substitution candidates); pack-expansion components `Dp<t>` are
  candidates. The template-args `J...E` wrap stays the deduced-run rule
  for concrete lists and the per-expansion rule for written lists.
- Totality: any written form outside the supported subset throws and
  the mangler falls back to today's concrete-signature spelling, so
  lowering never fails on an exotic pattern; the fallback path is kept
  byte-identical for signatures the new walk cannot handle.
- Layout: the shared mangling internals (`Substitutions`, component
  keys, type mangling) move behind `lowering/lower_name_parts.h`; the
  template-signature and written-form mangling lives in a new
  `lowering/lower_name_template.cpp` (file-audit headroom on
  `lower_name.cpp`).

## Status (2026-07-04)

COMPLETE: 176/176 pa22 tests pass and the root
`make test-report-through-pa22` gate is fully green (1907/1907).

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

The final batch landed: Itanium dependent-signature mangling (task 9,
including `Dp` pack parameters and written-form substitutable keys),
trailing-return decltype packs with the 14.8.2.4p8 pack ordering rule,
value-pack static-data-member definitions, decl-order-aware dependent
lookup (14.6.4 subset over binding sequence stamps), per-instantiation
dependent hidden friends with the class-scope signature lookup
context, recursive match-probe completeness demands as SFINAE,
explicit template-template arguments in unevaluated call deduction,
the init-helper anchor and by-address parameter conventions the
reference pins, pointer-arithmetic count widening, effect-free
implicit default-construction elision (with the odr-use preserved),
integral_constant-style conditional folding, pack-deduced delegating
temporaries, and the libstdc++ `__is_implicitly_constructible`
tuple-constraints gate as a dialect intrinsic (the reference resolves
that exact helper name to the enclosing specialization's leading bool
argument instead of running the body).

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
