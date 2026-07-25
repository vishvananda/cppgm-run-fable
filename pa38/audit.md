# PA38 Audit

## Audit Plan

Scope: the two PA38 commits (`bdc3b37ff` machine-IR optimizer, `ec07f9893`
LowIR `!dbg` round-trip) against `pa38/README.md` and `pa38/plan.md`.

Files to inspect:

- `dev/src/x86/mir_optimize.{h,cpp}` — the whole optimizer: def/use
  classifier soundness (implicit register roles, barrier conservatism),
  liveness dataflow, propagation/DCE fixpoint, zero-compare rewrite flag
  equivalence, branch-tail collapse, O2 layout, callee-saved prune and
  stack recompute.
- `dev/src/x86/mir_to_native.cpp` — MI_RET coalesced-register staging
  order against the epilogue restore sequence; prologue/epilogue
  callee-saved slot addressing consistency with `PruneCalleeSaved` and
  the stack-size recompute (alignment invariant, slot overlap with
  scratch, EH saved-reg table).
- `dev/src/x86/lowir_to_mir_program.cpp` — who populates
  `callee_saved_regs` (rbp/rsp must never be prunable) and the
  pre-existing `result_copy_hint` baseline cleanup (ownership boundary
  vs. the new optimizer's DCE).
- `dev/lowir2native.cpp` — `-O` parsing, required error-handling cases
  (no outputs, missing path, unreadable input, invalid LowIR).
- `dev/src/toolchain/compile_unit.cpp` — driver reuse of the same
  `OptimizeMirProgram` (the "not a display-only transform" rule).
- `dev/src/lowir/lowir_parser.cpp`, `dev/src/x86/lowir_to_mir_value.cpp`,
  `dev/src/x86/mir_serialize.cpp` — `!dbg` capture, stamping, and dump
  round-trip; no dump path regressions for location-free programs.
- `pa38/Makefile`, `pa38/scripts/*`, shared `scripts/` harness — confirm
  no local modifications, no weakened comparisons, no test-shape gates.

Performance risks to inspect:

- fixpoint loop cost: per-round `CollectSuccessors` recomputation
  (string-keyed map rebuilt up to 8 times per function);
- per-round instruction vector copying in `PropagateBlock` and
  `EliminateDeadCopies` (double copy via reversed `assign`);
- whole-block deep copies in `LayoutBlocks` reordering;
- `jmp *` all-blocks successor fan-out effect on liveness convergence.

Ownership boundaries to check:

- optimizer stays pure MIR-to-MIR (no LowIR or native knowledge);
- lowering keeps the `-O0` baseline shape (`result_copy_hint` cleanup)
  while `mir_optimize` owns all level-gated improvement;
- MIR dump and object/executable encoders consume the same optimized
  program (no hidden side representation);
- `!dbg` facts owned by `LowIRInstruction::debug_location` →
  `Instruction::debug_location`, not re-derived downstream; check the
  pre-existing `source_position` field does not duplicate this fact.

File-audit issues: run `perl scripts/cppgm_file_audit.pl --stage pa38
--paths dev/src`; confirm no size-limit dodges or code moved to
unchecked paths.

Cheating checks: no test-name/source-shape gates in the optimizer; no
interpreter/trampoline/embedded-payload substitutes (output must remain
real native code from the shared encoders); `-O1/-O2` must actually
transform the MIR rather than serializing precomputed shapes.

## Findings

Correctness (all verified sound, no fixes needed):

- `PruneCalleeSaved` cannot break EH snapshot frames: every function
  with landing pads (host, private-walker, or synthesized throw
  windows) gets `host_eh_enabled`/`host_eh_regions` in
  `AnalyzeEhRegions`, and `OptimizeFunction` skips exactly on that
  flag; as a second fence, `MI_EH_LANDING` classifies as a full
  barrier whose def mask makes `ExplicitMentionMask` keep everything.
- The preserve list only ever holds {rbx, r12..r15}
  (`FinishFrame`), so the prune can never drop rbp/rsp, whose
  `OP_FRAME` reads are invisible to operand mention scans.
- `SavedRegSlot` derives slot addresses from `stack_size` identically
  in prologue, epilogue, and `FinishEhFacts` CFI, so the pruned list
  plus the 16-byte-step `stack_size` recompute stays self-consistent,
  keeps saves below the fixed frame offsets, and preserves rsp%16
  call alignment.
- `cmp r, 0` → `test r, r` is flag-equivalent for all condition
  codes (CF=OF=0 both ways; AF is unused by jcc), and the `^ 1`
  condition inversion matches the paired `X86Condition` layout.
- The `MI_RET <reg>` staging (`mov rax, <reg>`) is emitted before the
  epilogue restores callee-saved registers, so coalescing onto rbx is
  safe; `-O0` never stages because baseline lowering returns via rax.
- `MirOperand` derefs are `[reg+disp]` only (no index register), so
  the frame-address fold cannot drop an index component.
- The `--batch-stdin` scaffold branch in `dev/lowir2native.cpp` is a
  non-runner fallback only: harness builds set `CPPGM_TEST_RUNNER=1`,
  which renames the tool main and links the shared `test_runner`
  fork-per-request main, so batch requests execute the real
  parse→validate→lower→optimize→emit path (verified by driving the
  built binary's batch protocol directly).
- No test-specific gates: every rewrite condition is structural
  (opcode/operand kinds); no function-name, file-name, or
  environment checks exist in `mir_optimize`.
- Harness and shared scripts are unmodified starter-kit files;
  `pa38/lowir2native-ref` is the stock symlink to the shared
  reference runner. Git history for `pa38/` contains only `plan.md`
  (and this audit record) beyond imports.
- `!dbg` serialization covers every opcode: all custom print paths
  (`shl/shr/sar_cl`, `call *`, `copy_bytes`, `zero_bytes`) append
  `dbg_suffix`, and every other opcode routes through
  `write_operands`, which appends it.
- Pre-existing, out of PA38 scope: `MirFunction::frame_bytes` is
  written by lowering but never read, and `MI_JNE` is an unemitted
  enum entry (the optimizer treats it conservatively).

Performance (fixed this audit):

- The ≤8-round propagation/DCE fixpoint copied every instruction up
  to three times per round: `PropagateBlock` rebuilt each block into
  a `kept` vector even when nothing changed, and
  `EliminateDeadCopies` built a reversed copy and then copied it
  back via `assign(rbegin, rend)`.
- `EliminateDeadCopies` also rebuilt the string-keyed CFG successor
  map every round, although the fixpoint never edits labels, deletes
  branches, or reorders blocks.
- `LayoutBlocks` deep-copied every `MirBlock` (all instructions,
  strings, operands) when reordering.

## Changes Made

All in `dev/src/x86/mir_optimize.cpp`, behavior-invariant:

- `PropagateBlock` rewrites in place and compacts with `std::move`
  only when a self-copy was dropped (no per-round copies otherwise).
- `EliminateDeadCopies` marks deletions in the backward liveness walk
  and move-compacts only blocks that lost instructions; it now takes
  the `BlockFlow` vector from the caller. `CollectSuccessors` runs
  once per function in `OptimizeFunction`; `ComputeLiveness` resets
  `gen/kill/live_out` so the flows are reusable across rounds.
- `LayoutBlocks` moves blocks through the layout permutation instead
  of copying them.

## Validation

- `make -C dev lowir2native cppgm++` — clean rebuild.
- `pa38 make test test-debuginfo` — 26/26 (o1 8/8, o2 10/10,
  debuginfo 4/4 + 4/4), pinned reference shapes unchanged by the
  cleanup.
- `perl scripts/cppgm_file_audit.pl --stage pa38 --paths dev/src` —
  pass (see below).
- Root `make test-report-through-pa38` — full ladder green (see
  below).
