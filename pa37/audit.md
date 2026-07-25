# PA37 Audit

## Audit Plan

Scope: the two PA37 commits (`efa4c7a93`, `804573bfb`) against `pa37/plan.md`
and `pa37/README.md`.

Files to inspect:

- `dev/lowiropt.cpp` — CLI surface: argument validation, error paths, no
  fallback-success exits.
- `dev/src/lowir/lowir_dump.{h,cpp}` — canonical dumper: presentation rules
  only, no semantic rewriting, no test-shape special cases.
- `dev/src/lowir/lowir_opt.{h,cpp}`, `lowir_opt_internal.h` — pipeline driver,
  CFG view rebuild cost, fixpoint loop bounds (risk: unbounded or quadratic
  rounds).
- `dev/src/lowir/lowir_opt_fold.cpp` — SCCP-style value pass: executable-edge
  soundness, slot-type tracking, no stringly encodings of value facts.
- `dev/src/lowir/lowir_opt_cse.cpp` — available-expression dataflow:
  intersection at joins, EH edge handling, key construction cost.
- `dev/src/lowir/lowir_opt_inline.cpp` — inliner: recursion/cycle guard, EH
  region guards, `__o1inlN__` naming contract, rename-map cost per site.
- `dev/src/lowir/lowir_opt_cfg.cpp` — CFG cleanup: EH structure guards,
  unreachable-block removal cost.
- `dev/src/lowir/lowir_opt_dce.cpp` — DCE: call-removal side-effect gates,
  slot-traffic analysis, escape detection.
- `dev/src/lowir/lowir_validate.cpp` — the two accepted-conversion changes:
  verify they widen validation only where LowIR semantics allow, not to mask
  bad optimizer output.
- `dev/cppgm++.cpp`, `dev/src/toolchain/compile_unit.{h,cpp}` — driver deltas:
  `LooksLikeLowIRText` sniffing (risk: source-shape acceptance gate),
  `CompileLowIRProgramToModule` shared path, `--emit-lowir -g` routing,
  `PruneUnreferencedLowIRDeclares`, `RemoveUnreferencedWeakFunctions`
  (risk: weak-discard removing referenced definitions).
- `dev/src/lowering/*` + `dev/src/sema/*` deltas — `trivial_lifecycle`,
  `unwind_declared`, `operator_` low-name spelling, memberwise assignment,
  wide zero-fill: check facts are represented in LowIR (PA37 boundary
  contract), not smuggled as side data; check no mode-gated behavior leaks
  into whole-program fixtures (PA32/PA36 memory).
- `dev/src/x86/lowir_to_mir_*` deltas — gpr_pair reclassification: confirm the
  backend derives it from LowIR types, not a leftover side channel.

Performance risks to check:

- pipeline rounds: program-level repeat-until-no-change × per-function
  fixpoint — confirm each iteration does real work and bails early;
- CFG view rebuild after every structural pass — frequency and cost;
- CSE key strings and per-block available-set copying;
- inliner body copies and rename maps on large callees;
- `RemoveUnreferencedWeakFunctions` / `PruneUnreferencedLowIRDeclares` — must
  not rescan the whole program per symbol (quadratic risk);
- `LooksLikeLowIRText` must not read whole files to sniff.

Ownership boundaries to verify:

- optimizer consumes only the parsed `LowIRProgram` (no frontend side data);
- object layer re-derives post-optimization facts from LowIR only;
- dump rules owned by `lowir_dump.cpp` alone (no duplicate serializer);
- unwind facts: single owner (`unwind_declared` from sema at lowering time;
  optimizer-inferred facts re-derived, not stored stringly).

Cheating patterns to rule out:

- test-name or source-shape gates anywhere in the pipeline;
- `-O1`/`-O2` silently falling back to `-O0` dumps on hard inputs;
- object roundtrip achieved by embedding/copying bytes instead of recompiling;
- validator changes that accept malformed IR the optimizer produces.

File-audit issues:

- all new files under 1500 lines / 120 columns (`lowir_opt_fold.cpp` at 1134
  is the closest); no hidden implementation fragments outside `dev/src`.

## Findings

Correctness (fixed):

1. **Multi-source `--emit-lowir -g<n>` failed outright.** The driver
   concatenated per-TU LowIR texts, so two TUs sharing an inline function
   died on `duplicate top-level symbol` — while the reference driver merges
   with vague-linkage dedupe (verified against `cppgm++-ref`: one weak
   `@twice` kept; declares yield to definitions; cross-TU inlining then
   works). The reference *miscompiles* internal-linkage collisions (it kept
   TU1's `static bump` body for TU2's caller and printed duplicate
   `@counter` top-levels); we instead rename colliding internals apart,
   which is valid, behavior-preserving, and inside the documented
   presentation latitude.
2. **Namespace-scope `static` functions lowered as `binding=strong`.**
   `nm` showed `T _Z4bumpv` where g++ emits a local symbol; two TUs with
   same-named statics would clash at link. Sema records `fn_static`; the
   lowering never consulted it (unnamed namespaces only).
3. **-O2 handler blocks ran with an empty EH-region stack** (fold pass), so
   a cleanup's stores never merged into the enclosing catch's slot state:
   `try { X x; foo(); } catch(...) { use(flag); }` with `~X()` writing
   `flag` folded the catch's load to the stale constant.
4. **-O2 promoted-slot dead-store elimination ignored cross-block
   exceptional liveness**: a store in block A observable only via a
   may-throw point in successor B's handler was deleted.
5. **CSE applied rewrites after a non-converged capped fixpoint** (64
   iterations, monotone-descending from TOP): on >64-block reverse-ordered
   CFGs a join could reuse an expression not computed on all paths.
6. **Float compares folded at long-double precision**; backends round each
   literal to f32/f64 first, so `cmp eq f32 16777217.0f, 16777216.0f`
   folded to a different truth value than the runtime compare.
7. **Signed-flavored folds on unsigned-typed operands used the
   type-canonical (zero-extended) value** where the CY86 reference lane
   reads the width pattern as signed (`div`/`mod`/`shr`/signed compares/
   `sext`/`sitofp` on u8/u16/u32 with the sign bit set); `uitofp` from
   signed types also read the sign-extended pattern instead of the
   zero-extended width image.
8. **Inliner pasted literal arguments into address positions** (`call
   @f(nullptr)` with a guarded `load` through the param produced malformed
   IR for the backend); its spill forwarding was also type-blind (an `f64`
   spill reloaded as `i64` forwarded the typed temp into a bit-image read),
   and no callee single-definition-discipline guard existed.
9. **Weak-discard reference scan diverged from the declare-prune scan**
   (missed `switch_values` globals and `tls_for`); the two walks were
   duplicated code. An alias whose object symbol is separately declared as
   a LowIR top-level could also lose its target.
10. **`--emit-lowir` output writes were unchecked** (ENOSPC exited 0 with a
    truncated file, on both driver-surface and classic paths).
11. **-O2 slot-state merges ignored the stored type**, so mixed-width
    stores of the same temp could fold a full-width reload.
12. **StripNoOpEh's region walk memoized its initial partial scan**, so a
    back edge re-entering the eh_try block skipped the leading
    instructions' may-throw check (contrived shape; fixed with an unmemoized
    initial scan plus a push-count depth cap).
13. UB hygiene: `-(long long)` negations at LLONG_MIN, unchecked
    out-of-range `long double`→`long long` casts in `sitofp`/`uitofp`.

Architecture / performance (fixed):

14. `RefreshInlineCycles` ran a per-function BFS (O(functions × edges) per
    program round) → replaced with one iterative Tarjan SCC pass.
15. `RemoveUnreferencedWeakFunctions` rescanned the whole program per
    removal wave (quadratic on weak chains) → refcount worklist over the
    shared collector.
16. CSE recomputed predecessors by scanning every terminator per block per
    iteration (O(blocks²)) → predecessor lists built once; its expression
    key was the rendered dump text → typed semantic-field key; dead `dups`
    plumbing removed.
17. `-c` slurped whole files to sniff LowIR → bounded 4KB prefix; the
    sniffer's `;`-comment skip (LowIR has no comments) removed; keyword
    match hardened to full identifier charset.
18. Inliner recomputed whole-function use counts per candidate site → one
    lazy `CountTempUses` per scan (shared with DCE); `MergePairs` left an
    emptied block with a fabricated valueless `return` for a later sweep →
    erased eagerly.
19. Dead code: `LowerProgram::optimize_level_` (zero readers — the
    level-independence contract is now structural), the orphaned PA33
    `LowerSimpleInlineConstruction`, fold's `visited`-duplicates-`exec`
    set, `(void)0` remnants, stale `lowir_unwind.h` doc pointers.
20. `trivial_lifecycle` was computed by string-matching reconstructed spill
    spellings (silently under-matched when slot names uniquify) → checks
    against the exact lines `EmitParameterStores` emitted.
21. The `operator =` → `operator_` low-name respelling was unconditional,
    textually drifting whole-program output from pinned refs (masked by
    harness canonicalization) → gated on separate compilation.
22. Host direct 9..16-byte object params over-approximated to GPR pairs for
    SSE-classified (float-bearing) classes; `LowerClassDirect` now admits
    only all-INTEGER shapes at that size, making the backend's pair rule
    complete from LowIR types alone.
23. 15.4p14 implicit exception specifications were wired only for
    synthesized assignments; synthesized default ctors, dtors, and built
    copy/move ctors now set `noexcept_decl` the same way, so host-mode
    definitions print `unwind=no` and the optimizer's EH strip/inline see
    the declared fact.
24. `const_eval_stmt` guarded `value_zero_fill` but not the new
    `value_zero_fill_wide` when treating `value` as an element offset
    (latent; both forms now guarded).

Explicitly examined and accepted (not defects):

- The dumper's `binding=strong` insertion before bare `prefer_local`, the
  inliner's continuation-split shape, and the inf/nan-only bit-image fold
  are documented reference-parity behaviors, not test gates; no test-name,
  environment, or source-shape acceptance gates exist anywhere in the PA37
  surface (verified by grep and by four independent review passes).
- `lowiropt` rejecting duplicate top-level symbols across multiple inputs
  matches the reference tool (probed).
- Merged-driver top-level order differs from the reference's interleaving;
  the harness canonicalizes top-level order by contract.
- Internal-linkage symbols keep course spellings (`_Z...`) rather than
  g++'s `_ZL...`; local symbols never resolve across objects.
- Stringly `metadata.find()` reads are the LowIR boundary contract itself
  (facts are validated metadata text; misspellings take the conservative
  branch).
- `LooksLikeLowIRText` cannot see through macros (`-Dglobal=int` plus a
  source whose first token expands): inherent to content sniffing on the
  documented `-c` LowIR-input surface; failures are loud parse errors.

Out of tracked scope (recorded, not deferred PA37 work):

- `make -C pa37 test-debuginfo` fails: the `!dbg` lanes were never
  implemented in this repo (the lexer tokenizes `!dbg`, the parser skips
  it, the model carries no locations, and `-gline-tables-only` has emitted
  no debug sections since the flag appeared in PA34 — verified with
  objdump; the test directories arrived in the course export with no
  accompanying implementation). This is a distinct assignment-scale
  feature (DWARF line tables + per-instruction source locations through
  the whole lowering + debug-preserving optimization contracts), not part
  of `make test-report-through-pa37` or any tracked stage gate, and it
  fails honestly — no fake-success path hides it.

## Changes Made

- `dev/src/lowir/lowir_merge.{h,cpp}` (new): linker-style unit merge for
  the driver emit surface — weak dedupe first-wins, strong supersedes,
  declares yield to definitions, colliding internals rename per unit;
  wired into `run_emit_lowir_driver_mode` (per-TU lower→parse→validate,
  merge, validate, prune, optimize, dump with checked writes).
- `dev/src/lowir/lowir_opt.cpp`: Tarjan SCC inline cycles; shared
  `CountProgramSymbolRefs` walker; worklist weak discard with the
  declared-alias pin.
- `dev/src/lowir/lowir_opt_fold.cpp`: handler-stack seeding at push sites
  (with conflict → `eh_stack_sane=false` fallback), float-compare rounding
  to operand type, signedness back-offs + `uitofp` width masking, typed
  slot-state merges, UB fixes, dead-code removal.
- `dev/src/lowir/lowir_opt_cse.cpp`: converge-or-bail cap, predecessor
  lists, semantic-field expression keys, dead plumbing removed.
- `dev/src/lowir/lowir_opt_dce.cpp`: `LiteralBarredPosition` (full
  storage-position list, shared), two-pass exceptional liveness for
  promoted dead stores, `CountTempUses` exported.
- `dev/src/lowir/lowir_opt_inline.cpp`: literal-position check in the
  paste, typed spill forwarding (both the callee planner and the caller
  cleanup), callee single-def guard, hoisted use counts, dead code gone.
- `dev/src/lowir/lowir_opt_cfg.cpp`: eager merged-block erase; unmemoized
  initial region scan with a depth cap.
- `dev/cppgm++.cpp` / `dev/src/toolchain/compile_unit.cpp`: prefix-bounded
  sniffing with full identifier charset, checked emit writes, dead
  `SetOptimizeLevel` plumbing removed.
- `dev/src/lowering/*`: static-function internal binding
  (`LowerOverloadInternalStatic`), separate-compilation gate on the
  low-name respelling, typed `trivial_lifecycle` spill handshake,
  all-INTEGER gate on host direct objects, dead PA33 expansion deleted,
  stale comments fixed.
- `dev/src/sema/*`: 15.4p14 `noexcept_decl` on synthesized default ctors,
  dtors, and built copy/move ctors; `value_zero_fill_wide` guard.
- `pa37/plan.md`: boundary description corrected (no re-derived unwind
  analysis), Architecture Review and Final Architecture Review added.

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa37 --paths dev/src`:
  **pass** (8 pre-existing warnings in older sema/parse headers, none in
  PA37 files).
- `make test-report-through-pa37`: **3408/3408, all 37 stages pass** with
  every fix applied (including the fixture-sensitive lowering changes).
- `make test-report ACTIVE_TEST_REPORT_PAS='pa37'`: 68/68 across o0/o1/o2,
  driver, and byte-exact object-roundtrip lanes.
- Live probes: multi-TU emit now matches the reference's weak-dedupe merge
  (`@twice` kept once; declare+definition merge; cross-TU constant folding
  reproduces the ref's `return i32 30`); internal-collision inputs compile
  correctly where the reference miscompiles; `nm` shows statics local
  (`t _Z4bumpv`); the A-S1 catch-handler load no longer folds to a stale
  constant; the S4 store survives as the exceptional path's `return 1`
  while the normal path folds to `return 2` — sound promotion on both
  paths.
