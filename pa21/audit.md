# PA21 Audit

## Audit Plan

Scope: PA21 work is `ab31396f0..HEAD` (~5,000 insertions, 55 files). The
required artifacts are LowIR text through the ordinary PA14-PA20 lowering
path; PA21 adds the template declaration/specialization model.

### Files to inspect

Template model (bulk of the new logic):
- `dev/src/sema/sem_member_template.cpp` (new, 485 lines) — member-template
  capture, per-specialization re-capture, out-of-class definitions.
- `dev/src/sema/template_body.cpp` (new, 517 lines) — pattern body re-walk.
- `dev/src/sema/sem_template.cpp` (+605) — capture and class instantiation.
- `dev/src/sema/sem_spec.cpp` (+568) — explicit/partial specialization,
  explicit instantiation, extern template.
- `dev/src/sema/template_deduce.cpp` (~850 lines churned) — unification,
  pack patterns, partial ordering.
- `dev/src/sema/template_args.cpp` (+258), `template_info.{h,cpp}` — argument
  resolution and the canonical entity graph.

Lowering (dummy-output / emission-policy risk):
- `dev/src/lowering/lower_unit.cpp` (+181) — demand loop, emission policy,
  explicit-instantiation ownership.
- `dev/src/lowering/lower_name.cpp` (+156), `lower_member.cpp`,
  `lower_global.cpp`, `lower_expr.cpp`, `lower_function.cpp`.

Supporting sema/parse changes:
- `sem_member.cpp` (+307), `sem_class.cpp` (+184), `sem_operator.cpp` (+118),
  `sem_pack.cpp` (+113), `sem_expr/sem_call/sem_cast/sem_convert/sem_ctor/
  sem_lifetime`, `const_expr/const_eval_expr`, `type_builder.cpp`,
  `ast_parse_class.cpp` (+102), `ast_parse_expr/ast_parse_names`.

### Cheat / regression checks

- No test-specific gates: no identifiers, file names, or source shapes from
  `pa21/tests/` matched in `dev/src`; no acceptance keyed to test fixtures.
- No fallback success paths: errors must exit failure, not emit partial or
  empty LowIR; no catch-all recovery that fabricates output.
- No interpreter/VM/trampoline/embedded-payload substitutes: PA21 output is
  LowIR from the ordinary lowering path; instantiated bodies must lower like
  ordinary functions.
- No timeout workarounds (sleep/alarm/retry loops) in place of algorithmic
  fixes.
- Regression sweep: `make test-report-through-pa21` covers PA1-PA20 suites;
  confirm all earlier stages green.

### Performance risks

- Specialization lookup: canonical-key maps vs. linear scans per template-id
  use; partial-spec candidate selection cost per lookup.
- Member-template re-capture: per-enclosing-specialization AST re-walk —
  bounded per instantiation, not repeated per use.
- Lowering demand loop in `lower_unit.cpp`: check for repeated full-table
  rescans per emitted symbol (quadratic in program size).
- Deduction: repeated substitution or full signature recomposition inside
  candidate loops.
- String-keyed identity: keys should be computed once per specialization,
  not rebuilt on every lookup in hot paths.

### Ownership boundaries

- `template_info.h` owns entity identity; source text must never carry
  identity (no stringly re-parsing of template-ids downstream).
- `sem_template.cpp` (capture/instantiation) vs `template_args.cpp`
  (params/args) vs `template_deduce.cpp` (unification/ordering) vs
  `sem_spec.cpp` (explicit/partial spec) vs `sem_member_template.cpp`
  (member layer) — check no duplicated selection logic across them.
- Lowering learns ownership only via `SemUnit::explicit_instantiations` and
  per-spec suppression flags — no sema decisions recomputed in lowering.

### File-audit issues

- Audit passes with 2 `bad-division` warnings (`parse/parser.h`,
  `sema/sem_binder.h`). Verify PA21 did not move implementation bodies into
  headers to dodge per-file checks (PA21 growth in `sem_binder.h` is
  declarations + comments; confirm).
- Check no oversized files were split into incoherent fragments purely to
  satisfy the audit, and no logic landed in unchecked paths (`pa21/`,
  scripts, generated files).

## Findings

Five parallel audit passes (template-model core, specialization/deduction,
lowering, supporting sema/parse, and a cross-reference cheat hunt) plus
direct verification of every raised claim:

1. Cheating / test-gaming: **clean**. No pa21 test identifier appears in
   `dev/src`; no embedded mangled-name tables, LowIR text payloads, or
   blobs; no getenv/argv/file-probe bypasses, no external-binary
   invocation, no sleep/alarm/signal tricks; `scripts/cppgm_file_audit.pl`,
   the Makefile timeouts, and all test/`.ref` fixtures are untouched in the
   PA21 range; exit codes are honest (errors throw and exit failure).
2. Lowering `object=@<symbol>` on weak local statics
   (`lower_unit.cpp:455`) was flagged as invalid metadata by one pass —
   **refuted**: the checked-in reference LowIR uses exactly this spelling
   as the weak merge key for hoisted local statics (e.g. the
   `binding=weak, object=@__local_static__...` globals in
   `pa21/tests/spec/*.ref`), and `pa13/lowir.md` defines `object=` as a
   free-form backend spelling carrier. No change.
3. Extern/explicit-instantiation dual-list ordering in
   `lower_unit.cpp:295-316` — **verified safe**: the extern path in
   `template_body.cpp` returns before registering an instantiation, and a
   definition after an extern declaration sets `inst_definition`, which
   the suppression loop checks before re-suppressing.
4. `sem_spec.cpp` ownership/perf (real, fixed): the matched partial's
   index was recovered via pointer arithmetic inside
   `InstantiateClassFromPartial` instead of being passed by the caller
   that already had it, and both partial-registration and
   member-definition matching recomputed `TemplateArgumentKey` for every
   existing partial per registration (quadratic in partial count).
5. `lower_unit.cpp` ctor-template entry scan (minor, fixed): the loop
   kept scanning after a match; at most one entry can match, so it now
   exits early.
6. Template-id friend matching (`sem_spec.cpp:150`) takes the first
   template the target deduces against. Multiple-match inputs need
   function-template partial ordering, which the PA21 handout lists as
   out of scope (undefined behaviour for this milestone; PA22 completes
   it). Within-boundary behavior is correct and deterministic
   (declaration order); not a defect.
7. Optimistic parameter pre-binding in `template_body.cpp:301-304`
   swallows exceptions by design; the full signature composition
   re-reports real errors, so nothing is masked — **verified, no change**.
8. Ready-member instantiation walks (`sem_template.cpp:767-923`) visit
   each (specialization, definition) pair once, guarded by
   `members_done`/`partial_members_done` — output-bound, not avoidable
   quadratic work.
9. File audit: pass with two pre-existing `bad-division` warnings
   (`parse/parser.h`, `sema/sem_binder.h`). PA21's `sem_binder.h` growth
   is member-function declarations and comments only — verified no
   implementation moved into headers or unchecked paths.
10. Regressions: none — the full `make test-report-through-pa21` gate
    (PA1-PA21, 1731 tests) is green, and only `pa21/plan.md` changed
    outside `dev/` in the whole range.

## Changes Made

- `dev/src/sema/template_info.h`: `PartialSpecialization` gains
  `pattern_key`, the canonical key of its pattern, computed once at
  registration.
- `dev/src/sema/sem_spec.cpp`: `RegisterClassPartial` compares cached
  keys instead of recomputing per candidate;
  `InstantiateClassFromPartial` now takes the matched partial's index
  (no pointer-arithmetic recovery).
- `dev/src/sema/sem_binder.h`: updated declaration for the index-taking
  signature.
- `dev/src/sema/sem_template.cpp`: partial-member definition matching
  compares `pattern_key`; the instantiation call site passes the index
  it already computed.
- `dev/src/lowering/lower_unit.cpp`: ctor-template entry scan stops at
  the first match.

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa21 --paths dev/src`:
  **pass** (2 pre-existing warnings, see Findings #9).
- `make test-report-through-pa21`: **1731/1731 green** with all audit
  fixes applied (exit 0).
- pa21 suite alone: 180/180 in ~2.1s wall — no timeout pressure, no
  slow-test outliers.
