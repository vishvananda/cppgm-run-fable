# PA23 Audit

## Audit Plan

Scope: the PA23 commits `4defba26e..82ebb8fcb` (~2,270 insertions across 38
files under `dev/`), reviewed against `pa23/plan.md`, `pa23/README.md`, and
the through-pa23 gate (2279/2279 green at audit start, 23/23 stages).

A second pass (loop 53) re-audited the full range `4defba26e..HEAD`
(2,313 insertions, 40 files) including the first pass's own fixes,
with fresh greps for test-keyed gates, env hooks, timeout
manipulation, swallowed exceptions, stringly facts, and unbounded
scans, plus a check that nothing outside `dev/` changed.

### Files to inspect

- `dev/src/sema/sem_apply.cpp` (new, 327 lines) — what unit is this? Verify
  it is a real ownership split, not an overflow bucket to dodge the
  1500-line source cap on `sem_expr.cpp`/`sem_member.cpp`.
- `dev/src/lowering/lower_fold.cpp` (new, 221 lines) — branch-fold analysis
  split out of `lower_unit.cpp` (ec45ffa73). Same real-unit check.
- `dev/src/lowering/lower_name_template.cpp` (+264) — decltype-expression
  mangling (cc55a7b07) and written function-type mangling (d50e38387).
  Check: manglings derive from typed AST/sema state, not from stored
  source text snippets; substitution-table behavior stays correct when the
  written form and the canonical form disagree.
- `dev/src/sema/template_args.cpp` (+264) — function-pointer NTTPs
  (fd692fd16), dependent value slot re-evaluation (8e62b9ad5). Check
  entity/linkage checks are 14.3.2-driven, not fixture-driven; check
  re-evaluation does not silently re-run arbitrary side-effectful folds.
- `dev/src/sema/template_deduce.cpp` (±335) — pack-prefix deduction
  (8f7fb57be), leading-explicit-pack runs (1b56964a1), non-deduced alias
  contexts (af3e21eba), target deduction under address-of (b443ea3ea).
  Check the pack-run split is a 14.8.2.1/14.8.2.5 rule keyed on structure,
  not a shape gate for specific fixtures.
- `dev/src/sema/sem_member.cpp` (±346) + `MemberCandidateSet` in
  `sem_expr.h` — member-call overload resolution split (41d7ebc0e).
  Perf risk: candidate composition re-deducing per call; quadratic
  candidate × argument re-analysis; copies of `SemValue` argument vectors.
- `dev/src/sema/sem_spec.cpp` (+108) — declared-signature explicit
  specialization selection (5c8aef7b9). Check selection matches by typed
  signature, not by written-string comparison.
- `dev/src/sema/sem_ctor.cpp`, `sem_lifetime.cpp`, `sem_class.cpp`,
  `sem_operator.cpp` — effect-free user-conversion elision (97817eb40) and
  syntactic base-chain elision (b76640bdf). Highest semantic risk: "elide
  the call when the class is empty and the body is effect-free" can be a
  ref-output-matching hack that drops required side effects (instantiation
  demands, static-member storage demands) or changes observable behavior
  in later execution stages. Verify gates are semantic (trivially
  effect-free bodies proven by sema) and that instantiation side effects
  still happen.
- `dev/src/sema/sem_template.cpp`, `sem_binder.{cpp,h}`, `scope.h` —
  static-member storage demand rules (58ab59fdd, 82ebb8fcb, plan cluster
  A). Check `lazily_instantiated` and the fold gate are typed state on the
  owning records, not string-keyed side tables; check demand loops do not
  rescan every specialization per read.
- `dev/src/lowering/lower_member.cpp` (+125), `lower_unit.cpp` (-337
  restructure), `lower_program.h` — comdat alias unit split (e80a67169),
  lone base-entry emission (40ef1286c). Check lowering still consumes
  sema-instantiated definitions only (no lowering-side re-derivation of
  sema facts).
- `dev/src/sema/template_body.cpp` (+153), `sem_class.cpp` — template-id
  friend classes through the instantiation seam (bce723181), friend
  merging (49417950f). Check merged friends share one entity record
  (no duplicated ownership between namespace template and friend decl).

### Cross-cutting checks

- Regressions: full `make test-report-through-pa23` (2293 tests) must stay
  green; file audit clean.
- Cheating patterns: grep for test-name/fixture-keyed gates, library-name
  keyed behavior beyond the audited PA22 intrinsic, early-exit success
  paths, output-shape switches on input text.
- Stringly facts: new `string` members/keys in sema records that encode
  types, values, or template arguments the type system already represents
  (`fn_name`/`written` uses are pre-existing; look for new ones).
- Performance: scans over all specializations/instantiations per
  expression; mangling recomputation per reference; repeated
  `test-report` timing vs pa22 baseline for the same suite.
- File-audit issues: the 3 pre-existing `bad-division` warnings
  (`parser.h`, `sem_binder.h`, `sem_expr.h`) — confirm the PA23 delta only
  added declarations/default no-op bodies to those headers, and that no
  implementation moved to headers or generated/unchecked paths.

## Findings

Fixed during this audit (none deferred):

1. **Conversion-elision hole + ownership violation** (97817eb40,
   `lower_member.cpp`). The empty-object conversion call-site elision
   decided effect-freedom by re-walking the callee's sema tree from the
   lowering, and its structural gate (lone `return T()` of an empty
   class) never checked the constructed temporary's constructor chain -
   an empty class with an effectful user default constructor would have
   its conversion call dropped along with the side effect. Both fixed:
   sema now analyzes the just-bound conversion body (where the
   `ClassRegistry` effect facts live), requires
   `!DefaultConstructionHasSyntacticEffects` on the returned temporary's
   chain, and publishes `SemNode.conversion_no_work`; the lowering only
   reads the flag.
2. **Dangling reference across re-entrant analysis** (82ebb8fcb,
   `sem_spec.cpp` `ResolveClassVariableTemplate`). A `ScopeBinding&`
   into the declaring scope's binding vector was held across
   `analyzer_.Analyze(init)` / `CopyInitialize`; an initializer naming
   another class-typed variable template in the same namespace re-enters
   `AddBinding` on the same vector and a reallocation dangles the
   reference (UB). The same window let the cached `var_specs` copy and
   the live binding diverge. Fixed by completing the binding in a local
   and registering it only after the initializer analysis; the cache
   copy and the scope binding are now written from the same fully-formed
   value.
3. **Demand-policy divergence** (58ab59fdd, `sem_template.cpp`). The
   "constexpr statics wait for odr-use" rule was enforced by
   `InstantiateStaticMembers(skip_constexpr=true)` at object-demand time
   but not by `InstantiateReadyMembers`, so a constexpr static whose
   out-of-class definition registered after the demand was instantiated
   anyway - the same fact encoded two ways, order-dependent extra weak
   storage. Fixed: `InstantiateReadyMembers` applies the identical
   constexpr carve-out.
4. **Stringly keyword stripping** (49417950f, `template_deduce.cpp`
   `SignatureReturnSpelling`). `friend`/`inline` were erased from the
   flattened return spelling by substring search with hand-rolled word
   boundaries - fragile against spellings that legitimately contain the
   words (e.g. string-literal text inside a decltype return). Fixed by
   filtering `SPEC_KEYWORD` specifiers (`KW_FRIEND`/`KW_INLINE`) at the
   AST level before flattening (`FlattenSpecifier` exported from
   `ast_text.cpp`, where it previously sat in the anonymous namespace).

Reviewed and judged sound (no change needed):

- **Spelled-prefix pack deduction** (8f7fb57be): deviates from vanilla
  C++11 (g++ 15 deduces the defaulted tail into the pack; the fixture
  and its checked-in ref pin the spelled prefix, object=
  `_Z4takeIiJiiEE...`). Reference dialect, keyed on typed structure
  (`spec_spelled`, trailing `pack_pattern`), general across programs.
  Documented in `plan.md`.
- **Deduction/NTTP cluster** (1b56964a1, af3e21eba, b443ea3ea,
  fd692fd16, 8e62b9ad5, 697ba068c, 57ee32692): structural rules per
  14.1p11/14.8.2.x/14.3.2p5; entity arguments carry typed identity
  (scope, name, `entity_fn_spec`); evaluation failures reject matches
  or throw - no fallback success paths.
- **Explicit-specialization selection** (5c8aef7b9) and
  **explicit-instantiation resolution** (18df66c45): typed
  `TypeEquals`/`TemplateArgumentKey` comparisons; unmatched forms are
  hard errors.
- **Member-call split** (41d7ebc0e): behavior-preserving refactor; no
  argument-vector copies per candidate, deduction once per template,
  no all-pairs loops; everything scales with overload-set size.
- **Static-member demand rules** (58ab59fdd/82ebb8fcb): scans bounded
  by the owner template's records, no whole-registry walks per
  expression; state typed on `ClassSpecialization`/`ScopeBinding`.
- **Comdat pairing / lone C2 emission** (e80a67169, 40ef1286c):
  reference emission conventions keyed on demand origin and
  specialization structure; presentation-level only.
- **Mangling** (cc55a7b07, d50e38387): decltype/written-form manglings
  derive from the typed AST; unsupported forms throw (honest
  EXIT_FAILURE); the only catch-fallback affects the `object=` pairing
  hint, which the relaxed compare strips.
- **Unit splits** (`sem_apply.cpp`, `lower_fold.cpp`, member-body
  helpers): cohesive real units in audited paths, not size-cap dodges.
- **Cheating sweep**: no test/fixture/library identifiers keyed in
  `dev/src`; no unconditional-success paths; grammar/timeouts
  untouched.

Second-pass findings (loop 53), re-verifying the above independently:

- The first pass's four fixes are real in the code: `conversion_no_work`
  is computed in `sem_member_body.cpp` from the just-bound body plus
  `DefaultConstructionHasSyntacticEffects` (memoized, recursive over
  bases and members) and `lower_member.cpp` only reads the flag;
  `ResolveClassVariableTemplate` completes its binding locally and
  registers after the re-entrant initializer analysis;
  `InstantiateReadyMembers` carries the constexpr carve-out;
  `SignatureReturnSpelling` filters `KW_FRIEND`/`KW_INLINE` specifiers
  before flattening.
- Nothing outside `dev/` changed in the whole PA23 range except the
  plan/audit docs and the `frontend_source_sets.mk` registration of
  the two new units — no earlier-PA refs, tests, scripts, Makefiles,
  or `pa23.gram` edits, and no timeout/sleep/alarm additions.
- Every catch block added by PA23 either rejects a candidate
  (SFINAE-correct `return false`/`continue`), falls back to a second
  evaluator then rethrows, or explicitly re-raises
  `InstantiationBodyFault` so genuine instantiation errors stay hard.
- New string-typed state is confined to mangler substitution tables
  in `lower_name.cpp`/`lower_name_template.cpp` (Itanium substitution
  keys are encoding-keyed by design) — no semantic facts became
  stringly.
- `InstantiateStaticMembers`/`DemandSpecializationStatics`/
  `OnStaticMemberReferenced` are bounded by the owning template's
  `member_defs`, the base chain, and the declaring scope chain
  respectively, all with done-guards — no whole-registry walks.
- The split units are registered in the build fragment and the parent
  files sit at 1193 (`sem_expr.cpp`) and 1346 (`lower_unit.cpp`)
  lines — real ownership splits, not cap dodges.
- **Doc defect (fixed)**: `plan.md` and this file mixed a stale
  `2293/2293` figure with the harness's actual `2279/2279` output;
  both normalized to the observed count.

## Changes Made

- `dev/src/sema/sem_member_body.cpp`: `ConversionBodyPerformsNoWork`
  (structure walk + syntactic effect check via `unit_.classes`)
  computed after each conversion body binds;
  `SemNode.conversion_no_work` published on the definition node.
- `dev/src/sema/sem_node.h`: the `conversion_no_work` fact, with the
  ownership comment.
- `dev/src/lowering/lower_member.cpp`: `ElideEmptyOperatorInit` reads
  the published fact; the lowering-side body walk is deleted.
- `dev/src/sema/sem_spec.cpp`: `ResolveClassVariableTemplate` builds
  the binding locally, runs the (re-entrant) initializer analysis, then
  registers and caches the finished binding.
- `dev/src/sema/sem_template.cpp`: `InstantiateReadyMembers` skips
  constexpr static-data definitions unless odr-use demanded them,
  matching `InstantiateStaticMembers`.
- `dev/src/sema/template_deduce.cpp`: `SignatureReturnSpelling`
  filters friend/inline specifiers at the AST level.
- `dev/src/ast/ast_text.{cpp,h}`: `FlattenSpecifier` moved out of the
  anonymous namespace and declared in the header.
- `pa23/plan.md`: Architecture Review and Final Architecture Review
  sections.
- Second pass: test-count figures in `pa23/plan.md` and this file
  normalized to the harness's actual `2279/2279` output; second-pass
  verification notes added to both documents. No code changes were
  needed — every code-level claim of the first pass held up under
  independent re-inspection.

## Validation

- `make test-report ACTIVE_TEST_REPORT_PAS='pa18 pa19 pa21 pa22 pa23'`:
  1042/1042 after each fix.
- `make check TEST=tests/spec/100-out-of-class-conversion-operator-definition.t`
  (the pinned conversion-elision fixture): PASS with the sema-owned
  fact.
- `make test-report-through-pa23`: **2279/2279, all 23 stages green.**
- `perl scripts/cppgm_file_audit.pl --stage pa23 --paths dev/src`:
  pass; the 3 `bad-division` warnings (`parser.h`, `sem_binder.h`,
  `sem_expr.h`) predate PA23 - the PA23 delta added only declarations
  and no-op virtual defaults to those headers.
- Second pass (loop 53): both gates re-run fresh —
  `make test-report-through-pa23` 2279/2279 (exit 0, 23/23 stages)
  and the file audit passing with only the 3 pre-existing
  `bad-division` warnings.
- Fixture-vs-g++ cross-check for the spelled-prefix dialect: g++ 15
  compiles the fixture but returns 1 (deduces the defaulted tail),
  confirming the checked-in ref pins a deliberate reference dialect
  rather than an accidental shape gate.
