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
- 37 pa23 failures: 20 LowIR reference mismatches and 17 unexpected
  EXIT_FAILUREs.

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
