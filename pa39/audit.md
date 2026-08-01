# PA39 Audit

## Audit Plan

Scope: everything the PA39 inception work changed between the PA38 audit
commit (`5e316b085`) and HEAD — 77 files under `dev/src`, ~180 new test
fixtures, and the pa39 plan/Makefile surface. The five required checks all
passed at the audit's start (fileAudit, test-report-through-pa38 3456/3456,
pa39 test-through-pa10, compare-pptoken-inception, compare-cppgm++-inception
`MATCH cppgm++`).

Checkpoints inspected:

1. **Ownership boundaries / regression risk against PA1-PA38.** Each of the
   27 ladder failures was fixed at a named earlier surface. Verify the fixes
   are mode-correct, that pinned fixtures were not weakened, and that each
   fix left a reducer in the earliest harness that can express it — or a
   written justification where none can (failures 13 and 20).
2. **Self-hosting special cases.** Sweep `dev/src` for inception/selfhost/
   PA39 conditionals, embedded payloads, interpreter/trampoline substitutes,
   or generated source-set scans replacing `dev/frontend_source_sets.mk`.
3. **Harness integrity.** Confirm no changes to `scripts/`, the root
   `Makefile`, or any `pa*/Makefile` in the PA39 range; confirm the pa39
   timeouts and RSS caps are course-provided values; confirm the
   object-level inception compare still gates every inception object.
4. **Reproducibility.** Flavor-invariant flags, shared content-compared
   generated config, no unstable order/timestamps/absolute paths reaching
   emitted objects; note what the byte compare proves.
5. **pptoken inception drift** fixed before the full compare.
6. **Performance risks.** The TransientScope release discipline, the
   deferred-noexcept resolution, the retry fixpoint, and the
   MarkCallCrossings loop extension.
7. **Code quality.** Stringly facts, ownership, hot-path recomputation,
   copying, file-size/file-audit gaming in the two refactor commits.
8. **Documentation coherence.** plan.md staleness; Architecture Review and
   Final Architecture Review grounded in the implementation.

Method: three parallel read-only reviews over the changed clusters (sema
template machinery; backend/MIR/toolchain; sema expr/lifetime/aggregate +
lowering), targeted greps for the special-case and reproducibility sweeps,
and empirical verification of every candidate finding against the current
compiler and `cppgm++-ref` before treating it as real.

## Findings

Every finding below was verified empirically (a failing probe against the
reference or a code-level trace) before being fixed; review claims that did
not verify are recorded under "Reviewed, no change" with the evidence.

### Fixed — wrong behavior

1. **Deferred noexcept facts never reached replay-bound definition nodes**
   (PA39 CWG-1330 machinery; regression introduced by the failure-16 work).
   An in-class member body of a class-template specialization binds inside
   the class's own replay window (`FlushDeferredBodies` runs from
   `CompleteClass` under `class_replay_depth_ > 0`), where
   `ComposedNoexceptSimple` reads false. The definition node then kept
   may-throw permanently while call sites resolved the binding's pending
   record to noexcept: the callee compiled without the 15.4p9 terminate
   barrier its callers assume. Verified: `V<int>::f() noexcept(true-const)`
   with a throwing body returned into the caller's catch (exit 1) where g++
   and the reference terminate (exit 134). Fix: definition nodes built
   in-window register a `PendingNodeFact` {node, expr, scope}; the facts
   resolve promote-only after the end-of-unit retry fixpoint
   (`ResolvePendingNodeFacts`, last step of `BindTranslationUnit`), before
   the lowering reads them; the retry heal path drops stale records before
   destroying a poisoned node; the third (namespace-scope) definition writer
   routes through the shared reader. Whole-program mode keeps its
   deliberate region-free noexcept shapes (PA36 architecture); the fact
   itself (`[unwind=no]`) now matches the reference in both modes.
   Reducer: `cppgm.tests/course/pa36/link/605-hosted-conditional-noexcept-
   member-terminate-runtime-smoke` (ref terminates, pre-fix we returned 1).
2. **Narrow signed frame homes fed 64-bit `sar`/`idiv` zero-extended**
   (PA28 surface, pre-existing; exposure widened by the PA39 staging
   changes). `emit_dest_copy` read an is_param frame home at its own width
   only when narrower than the consumer (equal widths fell through with no
   normalize), and read temp homes at the *operation* width instead of the
   canonical normalized i64 — `int f(int a){ try { return a >> 1; } ... }`
   returned 2147483644 for `f(-8)` in both whole-program and host modes.
   Fix: `emit_dest_copy` adopts `gpr_read`'s rule exactly — narrow param
   homes re-load at their own width and re-normalize unconditionally; every
   other frame home loads i64. All 106 pinned pa28 fixtures unchanged.
   Reducer: `cppgm.tests/course/pa28/210-narrow-signed-shift-div-frame-
   home.t` (shr/div/mod on negative i32 params parked by a crossing call).
3. **Hidden-friend fix incomplete on the merge path** (failure-22 surface).
   An ordinary namespace-scope function-template redeclaration beside a
   *concrete* hidden friend stayed invisible: the merge path never cleared
   `fn_templates_adl_only`, and the per-overload clear's `!binding->type`
   guard is always false for SB_FUNCTION bindings (they carry their first
   overload's type). Verified against the reference (accepts; we reported
   "undeclared name"). Fix: the merge path clears `fn_templates_adl_only`
   on any real (non-friend) redeclaration, like the create path. Reducer:
   `cppgm.tests/course/pa18/hidden-friend-template-redeclared-beside-
   friends.t`.
4. **Static/thread_local reference declarations were lifetime-extended**
   (regression introduced by the failure-8 work). `static const S& s =
   S();` attached a scope-exit destructor: whole-program mode then rejected
   previously-accepted code ("function-local static with a destructor"),
   and host mode registered `__cxa_atexit` on the reference's pointer slot.
   Fix: extension skips static/thread_local declarations — static-duration
   extension stays out of scope exactly like the already-documented
   namespace-scope case, matching the reference emission (which
   materializes the referee in a frame slot and never destroys it). No
   ref-fixture harness can express a reducer: exact-LowIR harnesses would
   pin the reference's no-destroy emission (ours legitimately destroys
   non-static bound temporaries), and hosted behavior smokes cannot observe
   the defect (the wrong atexit target and the shared dangling referee are
   invisible to a value check) — the failure-13/20 precedent applies; the
   shape is gated by whole-program emission wherever it appears.
5. **Statement-condition `&&`/`||` destroyed left-operand temporaries on
   the fall-through edge** (failure-25 surface, incomplete fix;
   pre-existing behavior retained by that fix). `if (Lock().ok() &&
   use())` released the lock before `use()` ran (g++ destroys after the
   whole condition; the reference defers destruction entirely, so fixtures
   never saw it). Fix: `LowerCondition`/`BranchOnValue` take per-edge
   cleanup flags; a short-circuit fall-through edge into the right operand
   keeps the accumulated temporaries alive, only condition-exiting edges
   run trampolines, and the registration list shrinks only when both edges
   consumed it. Reducer: `cppgm.tests/course/pa36/link/607-hosted-
   condition-and-temporary-order-runtime-smoke` (the destructor sets a
   flag the right operand reads — insensitive to the reference's deferred
   destruction, flipped by an early one).
6. **Class-static aggregate arrays with runtime init failed validation**
   (PA39 aggregate-array surface). `P H::t[2] = { { f(), 1 }, ... }`
   died with "argument count mismatch calling @P__P": the explicit
   element-address form was gated on `binding.home->kind ==
   SCOPE_NAMESPACE`, but a static member's home is its class scope, so the
   `@__cppgm_init` clone lowered the offset form with no object argument.
   Fix: the predicate covers class-scope static member definitions (both
   are emitted at namespace scope). Reducer: `cppgm.tests/course/pa36/
   link/606-hosted-static-member-aggregate-array-runtime-smoke`.
7. **`bind_failed` conflated post-bind member faults with bind failures**
   (failure-16 reset machinery). A fault escaping `InstantiateReadyMembers`
   set `bind_failed` without the record reset that pairs with it, so a
   later genuine bind failure of the same specialization would report
   softly (SFINAE-swallowable) instead of hard. Fix: the catch sites set
   `bind_failed` (and apply the soft-repeat downgrade) only when the bind
   itself was reset (`!spec.instantiated`).
8. **The end-of-unit retry loop could drop freshly-poisoned bodies.** The
   loop exited when a retry pass healed nothing, but
   `DrainPendingInstantiations` can poison *new* bodies, appending retry
   entries that would never run. Fix: the loop continues while either a
   body healed or the drain grew the queue; termination is bounded by the
   unit's finite body set (each pass heals or newly-poisons at least one).
9. **EH region imbalance for lifetime-extended temporaries.**
   `MaterializeClassResult` closed the open region and only reopened it on
   the registration path; a `lifetime_extended` result left the rest of
   the declaration's full expression uncovered. Fix: reopen without
   registering (conditional arms keep their pinned close-only shape).
10. **`WideReadPair` could fabricate a home for a malformed i128 read**
    (reads went through the alloc-on-first-sight `WideHome`); now throws
    on an unhoused operand. **`values_[name]` reads at width-normalization
    decision sites** (gpr_read, emit_dest_copy, the switch re-normalizes)
    now go through a checked `value_info()` — a missing record used to
    silently no-op the normalization.
11. **ELF personality edges.** `PersonalitySlotOffset` now reuses a
    defined `__gxx_personality_v0` symbol instead of always appending an
    undefined GLOBAL (a duplicate-symbol trap); the reader now verifies
    the legacy direct (0x1b) personality encoding by relocation name,
    symmetric with the indirect (0x9b) slot check it already had.
12. **Rehomed elaborated first-declarations displayed the wrong scope.**
    The 3.3.2p6 rehoming (failure 13) kept a display qualified by the
    syntactic scope; `TypeDisplayNameIn` now qualifies with the home the
    entity is actually declared in (no fixture pins the shape — the
    references mishandle the construct entirely).

### Fixed — structure (file-audit gate)

13. **The audit fixes themselves regressed the file-audit gate**: the
    pending-node-fact machinery pushed `sem_binder.h` to 1221 lines
    (limit 1200) and the width-rule fix pushed `emit_dest_copy` to 122
    lines (limit 120). Both fixed structurally, not by trimming:
    - The narrow-param frame-home width rule that `emit_dest_copy` had
      duplicated from `gpr_read` is now one shared helper,
      `emit_frame_home_load` (lowir_to_mir_value.cpp) — the "one rule"
      contract is one function, and `emit_dest_copy` is back to 106
      lines.
    - `sem_binder.h` follows the established state-header pattern
      (the SemLambdaState precedent): `SemPendingNodeFact` and the
      PA34 builtin shadow-template records (`SemBuiltinTemplates`)
      moved to `sem_binder_state.h`; the single-use `ClosureFunction`
      and `PendingClassDefinition` typedefs dropped; the stale
      "defined at the end of this header" `InstantiationContext`
      comment corrected to `sem_instantiation.h`; the
      `CurrentFunctionName` public/private sandwich removed and a
      misplaced `EnsureClassSpecialization` doc comment rehomed.
      1199 lines, declarations only, no behavior change (pa28's 106
      pinned MIR fixtures re-run byte-identical).

### Fixed — documentation/contract only

- `ReleaseScope` carries its ownership contract (the refusal set cannot see
  `ScopeBinding*` holders; callers re-home or refrain).
- The raw `also_in_rax` alias documents that RAX holds the un-normalized
  callee return and names the verified consumers.
- The r8/r9 argument-staging site documents that hoist safety rests on
  `MarkCallCrossings` (an under-approximation resurrects failure 26).
- `MarkCallCrossings` documents its def-before-use precondition and the
  O(regions²)-per-value cost bound (regions = backedges, small).
- `SN_CONSTRUCTOR_ACTION::value`'s two meanings (zero-fill span vs
  shared-base element offset) are documented at the field.
- The array branch of `MemberAssignAction` documents why only the
  synthesized-aggregate-ctor path can reach it.
- The compare path's own-width spilled load documents why only unsigned
  narrows reach a widened compare directly (signed narrows widen through
  a convert whose result is a normalized temp).

### Reviewed, no change (with evidence)

- **`eh_end` synthetic-window drain parentage** (review claim): the drain
  fires only when the region being closed sits *below* the window — the
  intended allocation-region close. Regions opened after the allocation
  push above the window and close LIFO before it is reached; the claimed
  premature-retire ordering is not constructible from the throw lowering's
  shapes.
- **Whole-program noexcept terminate regions**: deliberately absent (PA36
  "region-free pinned shapes" architecture), unchanged.
- **CWG-1330 scope pinning breadth/lifetime**: pinning the ancestor chain
  is required (evaluation walks parents); unpinning safely needs
  refcounting; the measured reclamation (-34% peak on the heaviest sema
  TU) was achieved with the pins in place.
- **`EvaluatePendingNoexcept` reader context**: the pending-spec contract
  is promote-only — a context-degraded evaluation yields a conforming
  may-throw reading, never a wrong promotion; libstdc++'s specs are
  context-free trait reads. Documented at the deferral block.
- **`ConstBodyRegistry` per-heal invalidation**: correctness requires
  in-pass visibility of healed bodies (retries legitimately read a
  sibling's constexpr definition healed earlier in the same pass); the
  rebuild cost is milliseconds per heal against multi-minute TU compiles.
- **Duplicate definition nodes after a failed-then-healed class bind**
  (review claim): publication happens at `CompleteClass`, the last step
  of the bind; the reset paths fire on pre-publication failures, and the
  `bind_failed` fix (finding 7) removes the post-publication reset
  arming. The residual window (a fault after CompleteClass inside
  BindDeclaration) produces analytically-identical duplicates and is
  covered by the failure-23 latent-diagnostic note in plan.md.
- **`EvaluateZeroArgConstantCall`'s by-name fallback**: the real shape
  (static constexpr members) resolves through `by_entity_` (same scope,
  unadjusted type); non-static zero-arg calls are never constant, so the
  display-name fallback decides nothing on this path.
- **`decl_using` copying pending noexcept records**: each copy clears
  itself before evaluating, so recursion terminates; the cost is one
  extra evaluation per imported overload, on a cold path.
- **while/do/for condition declarations**: per-iteration destruction
  remains an unimplemented, documented dialect boundary (failure-24 note);
  no `dev/src` file, checkpoint source, or fixture uses the form (grep
  verified).
- **12.2p5 subobject/comma-bound reference extension** and **class-element
  aggregate argument arrays**: documented boundaries from failures 8/10;
  the latter rejects loudly (`OutsideBoundary`).
- **`AnalyzeQualifiedMemberBody` poison-instead-of-throw under
  instantiation**: 14.7.1 no-diagnostic-required behavior, consistent
  with the failure-tolerant drain and the documented failure-23 note.
- **Inline-refusal breadth for EH callees, `HasEhInstruction` re-scan,
  `malloc_trim` per TU, aggregate `argarr` double-write**: each matches a
  documented contract (the argarr shape is the reference's own calling
  convention, confirmed by `--emit-lowir` probes) or is bounded by a
  gate that keeps it constant-factor.

### Audit sweeps that found nothing

- No inception/selfhost/PA39 conditionals in `dev/src` (comments only).
- `dev/frontend_source_sets.mk` unchanged; the Makefile errors on a
  missing list rather than scanning.
- No harness edits in the range: `scripts/`, root `Makefile`, and every
  other `pa*/Makefile` are untouched; the cppgm.tests diff is purely
  additive; `.my*` artifacts are gitignored, none tracked.
- pa39 timeouts (30s text tests, 900/3600s compile walls, 8 GiB RSS cap)
  are the original course-export values (`git log -S` verified).
- Reproducibility: both flavors compile with identical command lines from
  the same directory; the generated host config is shared and
  content-compared; the `-DCPPGM_DEFAULT_*` defines are flavor-invariant
  and consumed by no source file; no unordered-container iteration,
  pointer-keyed ordering, or timestamps reach emitted bytes in the
  changed code (reviewed file by file).
- The two file-audit refactor commits are cohesive moves (verbatim
  `mir_native_data` split implementing an existing interface; a
  self-contained `ResolveExplicitValueArgument` extraction), not
  size-limit gaming.
- All 24 reducers named in plan.md exist; failures 13 and 20 carry their
  documented no-reducer justifications.

## Changes Made

Compiler (all at the owning earlier surfaces):

- `sema/template_body.cpp`, `sema/sem_member_body.cpp`,
  `sema/sem_binder.{h,cpp}`, `sema/sem_template_check.cpp`: pending
  node-fact registration, drop-on-heal, end-of-unit resolution; retry
  loop drain-progress condition; `DeferredBody` copy elimination.
- `sema/sem_template.cpp`: `bind_failed` gated on the record reset.
- `sema/sem_spec.cpp`: merge-path `fn_templates_adl_only` clear.
- `sema/sem_lifetime.cpp`, `sema/sem_auto.cpp`, `sema/sem_binder.h`:
  extension skips static/thread_local declarations.
- `sema/sem_aggregate.cpp`: element-address form covers class-scope
  static member definitions.
- `sema/decl_binder.{h,cpp}`, `sema/sem_binder.{h,cpp}`:
  `TypeDisplayNameIn` for rehomed elaborated first-declarations.
- `lowering/lower_function.h`, `lowering/lower_expr.cpp`,
  `lowering/lower_eh.cpp`, `lowering/lower_member.cpp`: per-edge
  condition cleanups; lifetime-extended EH reopen.
- `x86/lowir_to_mir.h`, `x86/lowir_to_mir_value.cpp`,
  `x86/lowir_to_mir_flow.cpp`, `x86/lowir_to_mir_wide.cpp`,
  `x86/lowir_to_mir_analyze.cpp`: emit_dest_copy narrow/i64 rule
  (shared `emit_frame_home_load` with gpr_read), checked
  `value_info`, WideReadPair hardening, invariant comments.
- `sema/sem_binder.h`, `sema/sem_binder_state.h`, `sema/sem_spec.cpp`,
  `sema/sem_builtin_template.cpp`, `sema/sem_lambda.cpp`,
  `sema/sem_template.cpp`, `sema/template_body.cpp`: the finding-13
  state-header consolidation (SemPendingNodeFact, SemBuiltinTemplates)
  and typedef cleanup.
- `toolchain/elf_object.cpp`, `toolchain/elf_reader.cpp`: personality
  defined-symbol reuse; 0x1b verification.
- `sema/scope.cpp`, `sema/sem_node.h`, `sema/sem_class.cpp`: contract
  comments.

Tests:

- `cppgm.tests/course/pa28/210-narrow-signed-shift-div-frame-home.t`
- `cppgm.tests/course/pa18/hidden-friend-template-redeclared-beside-friends.t`
- `cppgm.tests/course/pa36/link/605-hosted-conditional-noexcept-member-terminate-runtime-smoke`
- `cppgm.tests/course/pa36/link/606-hosted-static-member-aggregate-array-runtime-smoke`
- `cppgm.tests/course/pa36/link/607-hosted-condition-and-temporary-order-runtime-smoke`
  (all with reference-generated fixtures)

Documentation: plan.md failure-21 header and memory-status brought
current; Architecture Review and Final Architecture Review added; this
audit record.

## Validation

- Probes (all verified failing before, passing after): the noexcept
  terminate smoke (exit 1 → 134, matching g++ and ref), the narrow
  shift/div reducer (exit 1 → 0), hidden-friend redeclaration
  (rejected → accepted, matching ref), static-reference declarations
  (whole-program boundary error → accepted), class-static aggregate
  arrays (validation error → correct values), the `&&` order smoke
  (release-before-use → g++ order).
- Suites re-run green after the fixes: pa28 106/106 + course 6/6 (no
  pinned MIR fixture shifted), pa18 193/193 + course 3/3, pa12 126/126 +
  course 2/2, pa24 94/94, pa36 69/69 + course 22/22.
- `make test-report-through-pa38`: green (see plan.md status; re-run in
  full after the audit changes).
- `perl scripts/cppgm_file_audit.pl --stage pa39 --paths dev/src`: pass.
- pa39 ladder: `make -C pa39 test-through-pa10`, `compare-pptoken-
  inception`, and `compare-cppgm++-inception` re-run from scratch against
  the audited compiler (results recorded in plan.md's status).
