# PA19 Audit

## Audit Plan

Scope: the six PA19 commits (`66958d893..97ad73694`) on top of the
audited PA18 baseline (`1983efb70`), reviewed against `pa19/plan.md`,
`pa19/README.md`, and the checked-in fixtures.

### Files to inspect

New sema files (full read):

- `dev/src/sema/template_args.cpp` — TemplateArg resolution, constant
  conversion, key/spelling value rendering.
- `dev/src/sema/sem_pack.cpp` — pack alias bindings, expansion
  drivers, `sizeof...`, 14.3p1 shape checks.
- `dev/src/sema/sem_spec.cpp` — explicit/partial specialization
  capture and selection, variable templates.

Changed sema files (diff review):

- `dev/src/sema/template_deduce.cpp` (+475/-~120) — value slots,
  trailing-pack deduction, partial-spec pattern deduction.
- `dev/src/sema/sem_template.cpp`, `template_info.{h,cpp}` —
  instantiation driver, TemplateParam/TemplateInfo model.
- `dev/src/sema/const_expr.{h,cpp}` — string-literal reads, functional
  casts, `TryClassConversionConstant` restricted conversion path.
- `dev/src/sema/type.{h,cpp}`, `type_builder.{h,cpp}`, `scope.h` —
  TemplateArg carrier migration, ConstValue move, pack bindings.
- `dev/src/sema/sem_binder.{h,cpp}`, `decl_binder.cpp`,
  `sem_expr.{h,cpp}`, `sem_member.cpp`, `sem_class.cpp`,
  `sem_lifetime.cpp`, `sem_cast.cpp`, `sem_new.cpp`,
  `sem_operator.cpp`, `sem_call.cpp`, `class_info.h`, `sem_node.h`,
  `sem_template_check.cpp`.

Changed lowering files (diff review):

- `dev/src/lowering/lower_name.cpp` (+83) — scope-path value
  spelling, specialization naming.
- `dev/src/lowering/lower_expr.cpp`, `lower_member.cpp`,
  `lower_convert.cpp`, `lower_unit.cpp`, `lower_program.h`.

Parser: `dev/src/ast/ast_parse_expr.cpp` (+21, `sizeof...`),
`ast_expr.h`.

### Cheating / fallback vectors to check

- pa19 harness (`pa19/scripts/compare_results.pl`, tests, Makefile):
  confirm untouched handout exports (checked: only `Export assignments`
  commits touch them — clean).
- Fixture-shape acceptance: grep for test-name, source-shape, or
  argument-count special cases in the new resolution/selection paths;
  the plan itself flags "the checked fixtures need exact-match" in
  `sem_spec.cpp` — verify the partial-spec matcher is a real
  structural matcher, not a fixture whitelist.
- `TryClassConversionConstant` ("restricted constexpr conversion for
  `B{}`"): verify it evaluates the real conversion-function body
  rather than pattern-matching a fixture shape and inventing a value.
- Dummy outputs: confirm no lowering path emits placeholder bodies or
  skips instantiation while still exiting success.
- Deferred static_assert: confirm dependent conditions are deferred by
  dependency analysis, not swallowed unconditionally.

### Ownership boundaries to check

- TemplateArg identity: key rendering (`TemplateArgumentKey`) versus
  spelling (`TemplateArgumentSpelling`) — identity must come from the
  typed model, not from re-parsed spellings (stringly-fact risk).
- Value-parameter constants: single owner for "this binding is a
  compile-time constant" (`ScopeBinding.has_value`) — check lowering
  does not re-derive constants from names.
- Pack state: `last_pack_param_` is mutable binder state threaded
  between signature composition and body binding — check for leakage
  across nested compositions.
- New-file ownership split (`template_args.cpp` / `sem_pack.cpp` /
  `sem_spec.cpp`) matches the plan's boundaries; no duplicated
  resolution logic left behind in `sem_template.cpp`.

### Performance risks to check

- Argument resolution and alias-scope construction per instantiation:
  look for repeated re-resolution of the same argument list, per-use
  re-evaluation of variable-template initializers (plan promises
  per-key caching), and repeated full-scope walks.
- Pack expansion: per-element transient scopes — confirm element
  iteration is linear, no re-expansion per mention.
- Partial-spec selection: linear scan over `partial_specs` per
  `EnsureClassSpecialization` is fine at fixture scale; check there is
  no scan nested inside per-argument loops.
- Key/spelling construction: check specialization lookups don't
  rebuild key strings quadratically in hot paths.
- `FindSpecializationRecord` / `DemandSpecializationStatics`: check
  these don't walk all templates/specializations on every use.

### File-audit issues to check

- `perl scripts/cppgm_file_audit.pl --stage pa19 --paths dev/src`
  passes with 2 warnings (`parser.h`, `sem_binder.h` bad-division) —
  both pre-date PA19; `sem_binder.h`'s PA19 growth is declarations
  only (verified by diff). Confirm no implementation bodies moved into
  headers or other unchecked paths during PA19.

### Exit criteria

- fileAudit: `perl scripts/cppgm_file_audit.pl --stage pa19 --paths dev/src`
- tests: `make test-report-through-pa19`

## Findings

### Fixed

1. **Fixture-driven partial-specialization selection**
   (`dev/src/sema/sem_spec.cpp`, `MatchPartialSpecialization`): the
   selector returned the *first declared* partial whose pattern
   deduced from the arguments, with a comment admitting the checked
   fixtures only need exact-match-vs-primary. With two overlapping
   partials (`pick<A*, B>` and `pick<A*, B*>` against
   `pick<int*, int*>`) declaration order decided the winner — wrong
   per 14.5.4.1. Fixed: all matches are collected and the most
   specialized wins via a new 14.5.5.2-subset ordering
   (`PartialAtLeastAsSpecialized` in `template_deduce.cpp`, reusing
   `SubstituteOrderingTypes` + `DeduceTemplateArgs`); a tie is an
   ambiguity error. Value slots keep slot identity, so `X<7>` beats
   `X<N>` and repeated-slot consistency (`X<N,N>` vs `X<N,M>`) falls
   out of slot-index equality.

2. **Silent zero-length expansion of abstract packs**
   (`dev/src/sema/sem_pack.cpp`, `ExpandPackExpression`): unlike the
   other expansion drivers, the expression driver did not check
   `PacksAreAbstract`; an abstract pack binding (empty `pack_args`)
   would silently expand `args...` to zero values instead of
   erroring. Unreachable today (pattern bodies are never analyzed at
   definition time), but the failure mode was silent wrong output
   rather than an error. Fixed with an explicit guard that throws.

### Verified non-issues

- **Harness integrity**: `pa19/scripts/`, `pa19/tests/`, and
  `pa19/Makefile` are untouched handout exports (git history shows
  only `Export assignments` commits).
- **`TryClassConversionConstant`** evaluates the real
  conversion-function body (single-return constant subset, base
  classes included) in class scope — a restricted evaluator, not a
  fixture-shape oracle.
- **Reference-pinned lowering shapes** (8.5p7 scalar zero-fill only
  for sizes 1/2/4/8, specialization braced value-init keeping its
  constructor call, `sizeof...` materializing through `const`,
  folded-const enum globals keeping the zero image, signedness-flip
  conversion copies) all key on semantic classes of programs, never
  on test names or source shapes; the checked-in refs are the
  assignment oracle for these presentation choices.
- **No dummy/stub/fallback paths**: every unsupported form throws
  (`OutsideBoundary`/`runtime_error`) and the driver exits
  `EXIT_FAILURE`; there is no fallback success path, interpreter, VM,
  or embedded-payload substitute anywhere in the diff.
- **Ownership**: template-argument identity lives in
  `TemplateArgumentKey` (canonical, never printed; entity pointers
  for uniqueness) and is separate from the display spelling; lowering
  reads value facts from the typed `TemplateArg`/`ScopeBinding`
  model (`folded_const` from `has_value`), never re-parsing strings.
  The old type-only resolution in `sem_template.cpp` was removed, not
  duplicated. New files match the planned boundaries and are
  registered in `dev/frontend_source_sets.mk`.
- **`last_pack_param_`** is reset before and consumed immediately
  after each signature composition at both consumers
  (`EnsureFunctionSpecialization`, `InstantiateFunctionBody`); stale
  state cannot leak across instantiations. Member-function parameter
  packs are outside the slice and fail with an error, not silently.
- **Mangling placeholder `L_DEPE`** for dependent-value arguments
  keeps mangling total; such signatures cannot deduce in this slice,
  and symbol names are canonicalized by the comparator, so no
  collision is observable.
- **Performance**: expansions are linear per element; variable
  templates cache per argument key (`var_specs`);
  `InstantiateReadyMembers` scans are guarded by `members_done`
  (pre-existing PA18 pattern); partial-spec matching is a linear scan
  per first instantiation only. No quadratic hot paths introduced.
- **File audit**: passes; the two `bad-division` warnings
  (`parser.h`, `sem_binder.h`) pre-date PA19, and PA19's header
  growth is declarations only (verified by diff).

### Reference-binary divergence (documented, intentional)

The reference binary accepts genuinely ambiguous partial
specializations (`pick<A*, int>` vs `pick<int*, B>` for
`pick<int*, int>`) by picking the first; the standard makes this
ill-formed and this implementation rejects it. Per the repo rules
(refs are imperfect; the handout and standard win on non-gated
inputs) the standard behavior is kept, so no course test pins the
ambiguous case; the most-specialized case, where the reference agrees,
is pinned by `cppgm.tests/course/pa19/300-partial-specialization-ordering.t`.

## Changes Made

- `dev/src/sema/template_deduce.cpp`: new
  `SemBinder::PartialAtLeastAsSpecialized` (14.5.5.2 subset over
  `TemplateArg` patterns).
- `dev/src/sema/sem_spec.cpp`: `MatchPartialSpecialization` now
  collects all matches, selects the most specialized, and throws on
  ambiguity.
- `dev/src/sema/sem_pack.cpp`: `ExpandPackExpression` rejects
  abstract pack bindings instead of silently expanding zero elements.
- `dev/src/sema/sem_binder.h`: declaration for the ordering helper.
- `cppgm.tests/course/pa19/300-partial-specialization-ordering.t`
  (+ `.ref` sidecars generated with the documented
  `make -C pa19 ref-test TEST=...` flow): pins most-specialized
  selection over three overlapping `pick` patterns.

## Validation

- `make -C pa19 test`: 120/120 spec+general, 1/1 course.
- `make test-report-through-pa19`: **1488/1488** (1487 baseline + new
  course test), pa1-pa19 all green.
- `perl scripts/cppgm_file_audit.pl --stage pa19 --paths dev/src`:
  pass (2 pre-existing declaration-weight warnings).
- Manual: the ambiguous-partials program exits `EXIT_FAILURE` with
  "ambiguous partial specializations of pick"; the ordering program
  compiles and its `static_assert`s select values 0/1/2 as 14.5.4.1
  requires.
