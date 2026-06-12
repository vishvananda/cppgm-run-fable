# PA15 Audit

## Audit Plan

Scope: the PA15 object-model slice, i.e. everything between the PA14 audit
commit (`4b14e8fb4`) and HEAD under `dev/` (~7.2k changed lines). Baseline at
audit start: `make test-report-through-pa15` green (1019/1019),
`cppgm_file_audit.pl --stage pa15` passes with one pre-existing `parser.h`
warning untouched since PA6.

### Files to inspect

Sema (new/grown):
- `dev/src/sema/class_info.h/.cpp` — ClassInfo registry + shared layout routine
- `dev/src/sema/sem_class.cpp` — deferred in-class analysis, access control
- `dev/src/sema/sem_member.cpp` — member access / method-call resolution
- `dev/src/sema/sem_ctor.cpp` — ctor/dtor selection + synthesis
- `dev/src/sema/sem_lifetime.cpp` — cleanups, temporaries, init/fini actions
- `dev/src/sema/sem_operator.cpp` — operator overloading + ADL
- `dev/src/sema/decl_binder.cpp/.h`, `sem_binder.cpp/.h`, `sem_expr.cpp/.h`,
  `sem_convert.cpp/.h`, `scope.h`, `scope_lookup.cpp`, `type*.cpp/.h`,
  `sem_node.cpp/.h`

Lowering:
- `dev/src/lowering/lower_member.cpp` — object addressing, method calls
- `dev/src/lowering/lower_unit.cpp` — demand-driven helper emission,
  `@__cppgm_init`/`@__cppgm_fini`
- `dev/src/lowering/lower_expr.cpp`, `lower_function.cpp/.h` — cleanups,
  eh regions
- `dev/src/lowering/lower_name.cpp/.h` — mangling extensions
- `dev/src/lowering/lower_program.h` — grown header, check for implementation
  bodies

Parser deltas: `ast_parse_expr.cpp`, `ast_parse_names.cpp`,
`ast_parse_declarator.cpp`, `parse_token.*`.

### What I will look for

1. **Cheating / shortcut paths**: test-specific or source-shape acceptance
   gates (string matches on test identifiers/file names), dummy or minimal
   LowIR output, fallback success paths that mask analysis failures,
   interpreter/VM/trampoline/embedded-payload substitutes for real lowering,
   timeout workarounds.
2. **Regressions**: weakened earlier-stage checks; behavior previously
   rejected now silently accepted; earlier-PA dump drift (gated by the
   through-pa15 report).
3. **Stringly semantic facts / ownership**: lowering re-deriving lookup or
   layout that sema should pin as typed `SemNode` facts; ctor/dtor identity or
   member offsets recovered from name strings downstream; duplicated layout
   ownership between `class_info` and lowering.
4. **Performance risks**: per-lookup linear scans over all classes/members
   (quadratic in program size), repeated full-`SemUnit` walks per function,
   hot-path recomputation of layout/mangled names, excessive value copying of
   class metadata.
5. **File-audit discipline**: implementation bodies hiding in grown headers
   (`sem_binder.h` +180, `lower_function.h` +69, `lower_program.h` +48,
   `sem_expr.h` +93), oversized files, fragments split only to dodge the
   audit, code moved to unchecked paths.

### Ownership boundaries to verify (from plan.md)

- `class_info` owns layout (offsets/size/align/bit-fields) — computed once at
  class completion, never recomputed in lowering.
- `SemBinder`/`SemExprAnalyzer` own all lookup/overload/access resolution;
  resolved facts (offset, base hops, callee entity, ctor/dtor variant) are
  typed `SemNode` state.
- `LowerProgram`/`FunctionLowerer` only consume resolved facts; mangling in
  `lower_name.cpp` is keyed by semantic entities, not source spellings.

### Method

Parallel read-only sweeps over the file groups above (cheat hunt, ownership,
performance, header/file-audit), each finding verified by direct reading
before any fix. Every confirmed blocker is fixed in this audit pass, then
re-gated with `make test-report-through-pa15` and the pa15 file audit.

## Findings

### Cheating / shortcut sweep — clean

- No test-specific gates: no string literals in `dev/src` match pa15 test
  names or fixture shapes; out-of-scope inputs reach `OutsideBoundary`
  hard errors (`EXIT_FAILURE`), never silent acceptance.
- No dummy/stub output, embedded LowIR/CY86 payloads, `.ref` reads,
  `system`/`popen`/exec, or interpreter/VM/trampoline substitutes anywhere in
  the PA15 diff. All previously-rejected forms that now succeed
  (class locals/globals, methods, bases, member access, ctors/dtors) have
  real lowering behind them.
- The two catch-based paths are principled, not fallback-success:
  - `AnalyzeCall` (sem_expr.cpp:497): on lookup failure, only the three
    `__builtin_*` names resolve from the builtin table; anything else
    rethrows the original error.
  - `TryVexingCallRecovery` (sem_lifetime.cpp:735): the 6.8p1
    statement/declaration disambiguation — fires only when the leading name
    resolves to a function (not a type) and the declarator has the exact
    vexing shape; otherwise falls through to ordinary declaration binding.
- No timeout workarounds, retry loops, or work caps.
- No weakened checks: nothing under `pa15/` (tests, refs, scripts),
  `scripts/`, or `Makefile` changed in the PA15 range except `pa15/plan.md`.

### Ownership / stringly facts — one acceptable exception, rest clean

- Layout is computed only in `class_info.cpp`; lowering addresses members
  exclusively from typed `SemNode` facts (`member_offset`, `base_hops`,
  bit-field unit/offset/width, `member_ref`) — verified in
  `lower_member.cpp`. No `Ast*` reference exists in `lowering/`.
- Ctor/dtor identity flows as `ESpecialFunction` enums end-to-end; the
  "C1"/"C2"/"D1"/"D2" strings are produced from the enum at the symbol
  boundary only.
- Reviewed exception: `LowerOverloadIndex` / `LowerMemberOverloadIndex` /
  `LowerOverloadDeleted` (lower_name.cpp:401-461) read the declaring scope's
  own binding via `FindOwnBinding` to compute the `__ov<N>` presentation
  suffix and per-overload deleted-ness. This is an entity-keyed read of
  sema-owned per-overload data (no scope-chain walk, no re-resolution) used
  for symbol naming and declare emission; dispatch is driven by `SemNode`
  facts. Judged acceptable and documented in plan.md.

### Performance — one fix, rest bounded

- **Fixed:** `LowerProgram::LowerUsedFunctions` (lower_unit.cpp) repeated
  whole-table sweeps until a pass made no progress — O(N²) entry scans in
  the worst case (backward weak-demand chains re-arm entries behind the
  sweep). Demand marking now goes through one chokepoint
  (`DemandFunction`) that records the lowest re-armed index, and each sweep
  restarts from that floor. Lowering event order is provably identical
  (entries below the floor are lowered or unchanged-ineligible), so output
  is byte-stable.
- Bounded and left as-is: `LowerScopeKey` string prepending is O(depth²)
  with tiny depth, once per entity; `FindClassField` and ctor-overload
  walks are linear in per-class member counts, once per class analysis;
  `DefaultConstructionHasEffects`/`DestructionHasEffects` recurse over the
  subobject tree once per synthesized-lifetime decision, not per program
  entity.

### Hygiene

- **Fixed:** dead helper `InnerObjectType` in lower_member.cpp (unused,
  `-Wunused-function`).
- **Fixed:** five `-Wreorder` constructor init-list orderings introduced by
  PA15 members: `Scope`, `ClassCtor`, `LowGlobalInfo`, `ImplicitConversion`,
  `SemNode`.

### File-audit discipline — clean

- `cppgm_file_audit.pl --stage pa15 --paths dev/src` passes; the single
  `parser.h` bad-division warning predates PA15 (file last touched in the
  PA6 audit).
- All PA15 modules are inside the audit limits (largest: sem_class.cpp at
  1110 of 1500 allowed lines; every grown header is under the 180-line
  body-weight threshold). No mechanical splits, no implementation outside
  `dev/src`, no audit-script or harness edits.

## Changes Made

1. `dev/src/lowering/lower_program.h`, `dev/src/lowering/lower_unit.cpp`:
   demand-floor rescan for `LowerUsedFunctions` — new `LowFunctionInfo::index`,
   `LowerProgram::DemandFunction` chokepoint (used by `FunctionRef` /
   `MemberFunctionRef`), and sweeps that restart from the lowest re-armed
   index instead of zero. Order-preserving; removes the repeated full-table
   walks.
2. `dev/src/lowering/lower_member.cpp`: removed the dead `InnerObjectType`
   helper.
3. `dev/src/sema/scope.h`, `dev/src/sema/class_info.h`,
   `dev/src/lowering/lower_program.h`, `dev/src/sema/sem_convert.h`,
   `dev/src/sema/sem_node.cpp`: constructor initializer lists reordered to
   declaration order (fixes all `-Wreorder` warnings).

## Validation

- `make test-report-through-pa15`: ALL TESTS PASSED (1019 / 1019) after the
  changes; earlier-stage dumps unchanged (the gate compares them).
- `perl scripts/cppgm_file_audit.pl --stage pa15 --paths dev/src`: pass
  (one pre-PA15 `parser.h` warning only).
- Rebuild of all tools is warning-free after the hygiene fixes.
