# PA21 Plan: Template Declaration and Specialization Model

## Goal

Finish the template declaration/specialization half of template completion on
top of the PA18-PA20 machinery: alias templates, variable-template extensions,
template-template parameters, member templates, friend templates, class
partial specialization completion (multi-pack patterns, dependent bounds,
function-type patterns), explicit-instantiation ownership, and PA19
explicit-specialization integration for members. Output stays LowIR through
the existing PA14-PA20 lowering path.

## Baseline

- 26/180 pa21 tests pass before this work; all 154 failures are pa21-local
  (no earlier-stage regressions).
- The PA18/19 model in `dev/src/sema/template_info.h` already carries:
  class/function/variable TemplateInfo, ClassSpecialization /
  FunctionSpecialization / PartialSpecialization records, single-pack
  parameter lists, structural deduction (`template_deduce.cpp`), partial
  ordering subset, and explicit specialization for namespace-scope entities.

## Design

### Ownership boundaries

- `template_info.h` owns the canonical template-entity graph: every template
  declaration form (class, function, variable, alias, member) maps onto one
  `TemplateInfo`; specializations are canonical-keyed records inside their
  primary's maps. Source-text forms never carry identity.
- `sem_template.cpp` owns capture and class instantiation;
  `template_args.cpp` owns parameter collection and argument resolution;
  `template_deduce.cpp` owns unification/ordering; `sem_spec.cpp` owns
  explicit/partial specialization; new `sem_member_template.cpp` owns the
  member-template layer (capture inside class bodies, re-capture at
  instantiation, out-of-class `template<..> template<..>` definitions).
- Lowering keeps its existing demand-driven weak emission;
  explicit-instantiation ownership talks to it only through
  `SemUnit::explicit_instantiations` (definitions) and per-spec
  `extern_suppressed` flags (declarations).

### Model extensions (task 1)

- `TemplateParam` gains `TPK_TEMPLATE` with its own nested
  `vector<TemplateParam>` (arity/kind matching only, per 14.3.3).
- `TemplateArg` gains `template_entity` (a `TemplateInfo*`) for
  template-template arguments; key/spelling/equality/dependence helpers learn
  the new form. A dependent template-template argument carries the bound
  parameter index (like `value_param`).
- Multiple parameter packs stay rejected on primary templates (14.1p11 makes
  them useless there) but are accepted on partial-specialization headers and
  function templates where each pack is deducible.
- Pack expansions inside partial-specialization patterns (`impl<list<T...>,
  Rest...>`) deduce structurally: a `pack_pattern` slot unifies against the
  remaining argument run of its template-id level.

### Alias templates (task 2)

- `TMPL_ALIAS` TemplateInfo captures `template<..> using X = type-id;` with
  the type-id AST as pattern. `X<args>` resolves by binding the args in an
  alias scope and resolving the type-id through the ordinary type builder —
  aliases never form their own specializations (14.5.7); the result is the
  substituted type. Inside abstract patterns the unsubstituted use stays a
  deferred type re-resolved at instantiation (recorded as the alias's own
  dependent-use binding carrying the AST part).

### Template-template parameters (task 3)

- Argument resolution for a `TPK_TEMPLATE` parameter accepts a
  template-name (class template, alias template out of scope per tests, or
  another bound template-template parameter) and stores `template_entity`.
- Alias scopes bind the parameter name as an `SB_CLASS_TEMPLATE` binding
  pointing at the argument's TemplateInfo, so `TT<int>` inside the pattern
  resolves through the ordinary template-id path.
- In abstract contexts the parameter binds a placeholder anchor entity whose
  `param_index` marks the slot; deduction unifies TK_TEMPLATE_SPEC patterns
  over it.

### Member templates (task 4)

- Class scopes record member templates in the owning `ClassInfo`
  (`member_templates`: name -> TemplateInfo, plus operator entries). Inside a
  non-template class the member template captures directly (declaring scope =
  the class member scope). Inside a class template pattern, the member
  template body is captured per-specialization when the enclosing
  specialization instantiates (re-walk of the member AST with the outer
  arguments aliased), so each ClassSpecialization owns fresh member
  TemplateInfos — canonical keys stay per-enclosing-specialization.
- Member-template calls: member lookup finding a member-template binding
  routes through the existing deduction machinery with `this`-adjusted
  signatures; instantiated bodies bind in the specialization's member scope
  (methods with the ordinary deferred-body path).
- Out-of-class `template<A> template<B> R C<A>::f(B)` definitions register on
  the enclosing class template as member-template definitions; on
  instantiation of the enclosing specialization they re-capture as the
  definition of the member template with the outer parameters aliased.
- Member class templates and member alias templates re-capture the same way.

### Friend templates (task 5)

- A `friend` template declaration inside a class (template) captures a
  namespace-scope function template marked ADL-only (reusing the PA12/PA17
  hidden-friend machinery); definitions carry the pattern body. In class
  templates, capture happens per enclosing specialization with outer
  arguments aliased (each instantiation redeclares the same namespace-scope
  template; bodies merge onto the first).
- Qualified friend declarations (`friend R ns::f<T>(..)`) only grant access:
  they resolve the named template and record it on the ClassInfo friend set.

### Explicit instantiation / extern template (task 6)

- Parser: `extern? template` also accepts special-member declarator forms
  (`extern template box<int>::box();`).
- Semantics per checked refs: an extern-template declaration suppresses
  local emission for the named specialization (functions: declare-only with
  strong binding; class: members not demand-emitted); an explicit
  instantiation definition emits with `object_root=yes`; a definition
  following an extern declaration for a *function* re-enables emission
  (object_root), while for a *class* the members emit on ordinary demand
  without object_root.

### Explicit specialization of members (task 7)

- `template<> R X<int>::f(..) {..}` claims the member's slot inside the
  already-created (or newly reserved) ClassSpecialization: the member binds
  as an ordinary strong definition in the specialization's member scope,
  replacing the pattern-instantiated body; forward uses re-check when the
  definition arrives (mirroring the function-template explicit_def path).
- Static-data-member specializations own their key like variable explicit
  specializations do today.

### Partial specialization completion (task 8)

- Pattern composition keeps dependent forms structural instead of erroring:
  dependent array bounds compose as value-slot bounds; function types with
  cv/ref-qualifiers and varargs compose; `pack_pattern` slots participate in
  deduction and ordering (a fixed tail after a pack maps like MapParamSpans).
- Ordering: pack slots order after fixed slots (14.5.5.2 via function-type
  ordering); value slots keep slot identity; cv on pointees/top-level
  distinguishes candidates.

## Status (2026-07-04)

COMPLETE: 180/180 pa21 tests pass; through-pa21 fully green
(1731/1731); file audit clean.

Landed on top of the earlier member-template/alias/friend/explicit
-instantiation work: template-template argument mangling (nested
template names, `J..E` parameter-pack wrapping), array functional
casts (`A{pack...}` temporaries with discarded-array materialization),
the 14.2p4 dependent member template-id keyword check (definition-time
sanity walk), the 13.5p6 operator-specialization operand check,
extra-base call projections, weak hex-named local statics inside
vague-linkage definitions, instantiated bodies never counting as
definition-time demand (14.7.1), the fn-pointer image policy (class
images go dynamic, array images stay static), C-style function-type
argument spellings, the empty-class operator-init elision, work-free
aggregate-member temporaries, direct call-result transfer in trivial
copies, template-id friends (`operator+<>`) pinning existing template
specializations, spec-identified template-id static calls,
using-imported member templates with 7.3.3p15 hiding, alias pack-slot
forwarding through substitution, folded static-member reads demanding
weak storage outside instantiated bodies, qualified super-call
two-stage base projections, reference-lvalue compound-assignment
store re-derivation, inline out-of-class ctor weakness, and concrete
pack expansions filling fixed template parameters (14.3p1).

## Architecture Review

Audit of the landed implementation (range `ab31396f0..HEAD`, ~5,000
insertions over 55 files) against the design above:

- Entity graph: identity lives where the design put it.
  `template_info.h` holds the only identity-bearing records
  (`TemplateInfo`, `ClassSpecialization`, `FunctionSpecialization`,
  `PartialSpecialization`); specialization identity is
  `TemplateArgumentKey` over typed arguments (entity pointers for
  template-template arguments, canonical types for type arguments,
  PA20 constant values for value arguments). No source-text form
  carries identity. The pre-PA21 text-canonicalization helpers
  (`PositionalizeTemplateNames`, `CanonicalDeclaratorParams`) remain
  confined to out-of-class-definition redeclaration matching and are
  not used for specialization identity or selection.
- Ownership split held: capture/class instantiation in
  `sem_template.cpp`, member-template layer in
  `sem_member_template.cpp`, explicit/partial specialization and
  extern-template in `sem_spec.cpp`, parameter/argument resolution in
  `template_args.cpp`, unification/ordering in `template_deduce.cpp`,
  instantiation-demand bookkeeping in `template_body.cpp`. The audit
  found no duplicated selection or deduction logic across these
  units.
- Lowering boundary held: lowering consumes
  `SemUnit::explicit_fn_instantiations` /
  `extern_fn_suppressions` and the per-spec `extern_suppressed` /
  `inst_definition` flags; the definition-after-extern case is
  ordered correctly because the suppression loop skips specs whose
  `inst_definition` was set by sema. Weak local statics inside
  vague-linkage bodies carry `object=@<symbol>` — verified against
  the checked-in reference LowIR, which uses exactly that spelling as
  the weak merge key.
- Member templates re-capture per enclosing specialization, as
  designed: the pattern re-walk binds under a fresh specialization
  scope, so each `ClassSpecialization` owns fresh member
  `TemplateInfo`s; `members_done` / `partial_members_done` /
  `member_spec_names` keep every (specialization, definition) pair
  instantiated exactly once, with explicit member specializations
  masking pattern definitions.
- Audit fixes applied on top of the landed work: partial
  specializations now cache their canonical `pattern_key` at
  registration (redeclaration merging and member-definition matching
  no longer rebuild candidate keys in loops);
  `InstantiateClassFromPartial` takes the matched partial's index
  from the caller instead of recovering it by pointer arithmetic;
  the ctor-template entry scan in `lower_unit.cpp` stops at the
  first match.

## Final Architecture Review

Exit state against the Stage Handoff contract:

- Stable graph: templates, partial specializations, explicit
  specializations, and explicit instantiations resolve onto shared
  records; `class_specs`/`fn_specs` are ordered maps keyed by
  canonical argument keys, so selection and emission order are
  deterministic for identical inputs.
- Specialization selection is deterministic: partials are kept in
  declaration order and matched via the PA19 most-specialized
  ordering; the chosen partial's index and bound slots are recorded
  on the specialization (`partial_index`, `partial_bound`).
- Ownership lowers through the ordinary path: explicit-instantiation
  definitions emit as object roots, extern declarations suppress
  emission, definitions after extern re-enable it — all through
  sema-owned flags; no lowering-side re-derivation of template facts
  beyond mangled-name construction from the entity records.
- No "template entity model later" gap remains: alias templates,
  variable templates, template-template parameters, member/friend
  templates, and current-specialization identity are all modeled.
  What PA21 intentionally leaves for PA22 is exactly the README's
  out-of-scope list (full deduction, function-template partial
  ordering, SFINAE); the one visible seam is template-id friend
  matching, which takes the first deducible template — inputs where
  several templates match need partial ordering, which the handout
  defines as undefined behaviour for this milestone and PA22
  completes.
- Performance: the 180-test pa21 suite runs in ~2.1s wall; audit
  removed the only avoidable quadratic registration scans (cached
  pattern keys). The ready-member instantiation walk visits each
  (specialization, definition) pair once, which is proportional to
  the work it must produce.

## Validation

- Fast loop: `make test-report ACTIVE_TEST_REPORT_PAS='pa21'` and
  `make -C pa21 check TEST=tests/...` for single cases.
- Gate: root `make test-report-through-pa21` after each feature lands (the
  earlier suites must stay green — template machinery is shared from PA18 on).
- File audit: `perl scripts/cppgm_file_audit.pl --stage pa21 --paths dev/src`
  (keep new code in cohesive files; split member-template layer into its own
  translation unit).
