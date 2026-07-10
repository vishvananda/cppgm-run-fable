# PA23 Plan: Template Feature Integration

## Goal

Make the PA18-PA22 template features compose in realistic combinations, with
LowIR output unchanged in format. PA23 adds no new isolated feature; every fix
threads existing typed template state (specialization records, deduction
bindings, instantiation records, member-definition registrations) through
combined cases without special-casing individual tests.

## Baseline

- Through-pa22 is green (2242/2279 report tests; every pa23 failure is
  pa23-local).
- 37 pa23 failures at the start of this pass: 20 LowIR reference
  mismatches and 17 unexpected EXIT_FAILUREs.

## Status

COMPLETE: 2279/2279 through-pa23; file audit passes. All clusters
landed: static-member storage/fold rules, partial-spec structural
matching, deleted-template SFINAE, function-pointer NTTPs,
explicit-instantiation forms, ADL template-ids, pack-prefix and
dual-pack deduction, non-deduced alias contexts, declared-signature
specialization selection, friend-template merging, written
function-type + decltype-expression mangling, effect-gated
user-conversion elision, and class-typed member variable templates
(storage + template-id object mangling + init-order anchor).

Known convention gap for later stages: our variable-template-id
object name spells the class-spec argument by full-prefix
substitution (`_ZN4propILi0EE14static_query_vI2exS0_EE`, the g++
form) while the reference spells it via injected-class-name
(`...I2exS_ILi0EEE`). The relaxed compare strips object= metadata, so
this only matters if a later execution stage links our objects
against reference-mangled symbols.

## Failure taxonomy (from the checked-in refs and diagnostics)

- **A. Static-data-member storage semantics** (5 diff tests):
  `100-dependent-bool-partial-static-value-mangle`,
  `100-intermediate-type-transform-value-nontype`,
  `100-structured-bool-boost-convertible-mpl-overload`,
  `500-dependent-qualified-sizeof-static-member`,
  `200-class-partial-specialization-no-derived-base-deduction`.
- **B. Over/under-instantiation of special members** (extra or missing
  `<fnN>` bodies): `100-dependent-qualified-nontype-base-argument`,
  `300-dependent-bool-base-trait-type-argument`,
  `500-dependent-function-type-pack-expansion-ctor-init`,
  `100-static-member-on-explicit-specialization`,
  `spec/100-explicit-instantiation-class-prior-member-definitions`.
- **C. Wrong overload/specialization selected**:
  `100-explicit-function-specialization-overload-parameter-match`,
  `200-member-template-implicit-instantiation-not-overload`,
  `spec/300-deleted-function-template-*` (deleted callee must be a
  SFINAE failure), `400-member-variable-template-leaf-sfinae`,
  `spec/100-out-of-class-conversion-operator-definition`.
- **D. Function-pointer non-type template parameters** (4 tests):
  `100-nontype-function-parameter-adjustment`,
  `spec/100-function-template-nontype-function-pointer-{call,specialization-call}`,
  `spec/100-nontype-function-pointer-argument`.
- **E. Rejected-but-valid compositions** (remaining EXIT_FAILUREs): explicit
  instantiation matching (`extern template` of static data, template-id with
  deduction), ADL with explicit template-id, partial specializations
  distinguished by function-type arity vs pack, qualified member-template
  calls through enable_if, reentrant static queries, dependent
  qualified/friend member access.

## Cluster A design: static const member storage

The fixtures pin these rules (cross-checked against pa18-pa22 refs:
`Box`, `value_source`/no-storage, `ratio` constexpr, `Deque::block_size`,
`integral_constant` object case):

1. Storage exists only for members with a registered out-of-class
   definition. In-class-initializer-only members never emit.
2. Object creation of a specialization type (variables — existing rule —
   and now also constructed temporaries) demands all statics of the
   base-subobject chain.
3. Completing a conversion endpoint that is a dormant specialization
   (14.7.1p4 path) also demands its statics: parameter/conversion types
   of overload candidates get storage even when the candidate loses.
4. A folding read demands storage when the owning specialization was
   itself instantiated lazily inside template instantiation; a folding
   read of a parse-scope-instantiated specialization leaves no storage
   (the pa19 `value_source<int,-100>`/`traits::const_min` shape).
5. `constexpr` members never get storage from folding reads (the pa21
   `ratio` shape); odr-use (non-folding reference) still instantiates
   their definition (the pa21 address-pack shape).
6. A member whose in-class declaration has no initializer does not fold
   on reads even after its out-of-class definition instantiates: readers
   load the weak global, and a namespace-scope constant initialized from
   it gets dynamic init (the `bucket_array_base` shape). The definition
   itself still emits its constant-evaluated initializer.
7. A demand instantiates all registered statics of the specialization,
   not only the named member (`sizes` + `sizes_length`).

Ownership: `ClassSpecialization` records `lazily_instantiated`;
`SemBinder::OnStaticMemberReferenced`/`InstantiateStaticMembers` own the
demand rules; `DeclBinder::RecordConstantValue` and
`FinishConstexprObject` own the fold gate; `MakeTemporaryObject` and the
conversion completion hook feed the object-demand rule. Lowering is
unchanged: it keeps emitting whatever member definitions sema
instantiates.

## Cluster B/C/D/E approach

Each cluster gets the same treatment: reduce the first failing test,
identify the typed state that is lost between the composed features
(per the assignment design notes), fix it in the owning subsystem
(deduction in `template_deduce.cpp`, candidate viability in
`sem_call.cpp`/`sem_convert.cpp`, instantiation timing in
`sem_template.cpp`, NTTP argument resolution in `template_args.cpp`),
and re-run the earlier template PAs with pa23.

D extends `template_args.cpp` value arguments to pointer-to-function
constants (entity + linkage checks per 14.3.2), reusing the PA22
address-valued argument model; mangling reuses the PA22 Itanium
scheme.

## Validation

- Fast loop: `make test-report ACTIVE_TEST_REPORT_PAS='pa23'` and
  `make -C pa23 check TEST=...` for single cases.
- After each sema/lowering change:
  `make test-report ACTIVE_TEST_REPORT_PAS='pa18 pa19 pa21 pa22 pa23'`.
- Gate for every commit: root `make test-report-through-pa23` stays
  monotonically improving; any older-PA regression is fixed before
  continuing.
- `perl scripts/cppgm_file_audit.pl --stage pa23 --paths dev/src` stays
  clean.

## Architecture Review

What PA23 actually is in the tree: no new subsystem, ~2,270 lines of
integration fixes threaded through the PA18-PA22 template layers, plus
three real unit splits (`sem_apply.cpp` conversion application out of
`sem_expr.cpp`, `lower_fold.cpp` branch-fold analysis out of
`lower_unit.cpp`, member-call candidate composition phases in
`sem_member.cpp` with the shared `MemberCandidateSet`).

State ownership held up: every new fact landed typed on its owning
record rather than in side tables - `ClassSpecialization.
lazily_instantiated` and `statics_demanded` (demand rules),
`ScopeBinding.value_from_def` (14.6.4.1 fold visibility),
`ScopeBinding.var_spec_template/var_spec_args` (variable-template
identity the lowering mangles from), `TemplateArg.entity_fn_spec`
(function-pointer NTTPs), `NamedTypeInfo.spec_spelled` (spelled-prefix
deduction), `SemNode.demand_strong` / `user_conversion` /
`conversion_no_work` (emission and elision facts). Lowering consumes
these; it does not re-derive sema facts (the one violation found - the
conversion-elision body walk - was moved into sema during audit).

Deduction changes are structural rules, not fixture gates:
pack-run separation keys on `CollectPatternPackSlots`, non-deduced
alias contexts on the anchor record's `TMPL_ALIAS` kind, explicit
specialization selection on composed-signature `TypeEquals`,
derived-to-base deduction restricted to call contexts per 14.8.2.1p4
vs 14.5.5.1.

Reference-dialect divergences from vanilla C++11 are deliberate,
pinned by checked-in refs, and implemented generally:
- Spelled-prefix pack deduction (`spec_spelled`): `tuple<T0, Ts...>`
  against a defaulted ten-parameter `tuple<int,int,int>` deduces
  Ts={int,int} (ref object= mangling `_Z4takeIiJiiEE...`); g++ 15
  deduces the defaulted tail into the pack (sizeof...=9). Keyed on
  typed structure (trailing pack pattern, spelled-arg count), not on
  test shapes.
- Empty-object operator/conversion call-site elision and the C1/C2
  comdat pairing rules mirror the reference's emission conventions;
  the elision is gated on sema-proven effect-freedom.

## Final Architecture Review

Audit-driven changes (see `pa23/audit.md`): the conversion-elision
fact moved from a lowering-side body re-walk into sema
(`ConversionBodyPerformsNoWork` in `sem_member_body.cpp`, published as
`SemNode.conversion_no_work`), and now also requires the returned
temporary's construction chain to be syntactically effect-free -
closing the hole where an empty class with an effectful user default
constructor could have its conversion call dropped. Signature-identity
keyword stripping (`friend`/`inline`) moved from post-flatten string
erasure to AST-level specifier filtering. A dangling-reference hazard
in `ResolveClassVariableTemplate` (binding reference held across
re-entrant initializer analysis) was restructured to complete the
binding locally before scope registration, which also removed the
window where the cached copy and the live binding could diverge. The
constexpr-static demand policy is now enforced identically at demand
time and at late-definition registration (`InstantiateReadyMembers`
skips constexpr statics exactly like `InstantiateStaticMembers`).

Remaining known-convention gap (unchanged, documented above): our
variable-template-id object name spells the class-spec argument by
full-prefix substitution (g++ form) where the reference uses the
injected-class-name form; relevant only if a later execution stage
links against reference-mangled object symbols.

Handoff state for PA24: template semantic layer complete and composed
(2279/2279 through-pa23, 23/23 stages), instantiated declarations
lower through the ordinary LowIR path, no template-subset
special-casing in lowering, file audit clean.

A second audit pass (loop 53) independently re-verified the first
pass's conclusions against the full PA23 range (`4defba26e..HEAD`):
no earlier-PA test, ref, script, Makefile, or grammar file changed;
no timeout manipulation; every added catch block rejects a candidate
or rethrows (`InstantiationBodyFault` is re-raised, so real
instantiation errors are not swallowed as deduction failures); the
new string keys are all mangler substitution-table state; the demand
loops stay bounded by the owning template's member list; and the two
unit splits register in `frontend_source_sets.mk` with post-split
sizes (1193/1346 lines) well under the cap. The only defect found was
a stale test-count figure in these documents, corrected to the
harness's actual 2279/2279 output.
