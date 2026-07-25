# PA38 Plan: machine-backend optimization levels (`lowir2native -O1/-O2`)

## Contract

PA38 optimizes the typed machine IR after the PA28 lowering boundary and
before native emission. `-O0` stays the untouched PA28 baseline. `-O1`
performs local (block-scoped rewrites + whole-function dead-copy removal)
cleanup; `-O2` adds whole-function layout and frame work. Both the
`--dump-machine-ir` dump and the executable/object encoders consume the same
optimized `mir_model::MirProgram` — there is no hidden side representation.

The structural oracle (`x.ref.cmir`) canonicalizes only free-pool GPR names
(`rbx`, `r10`..`r15`), free XMM names (`xmm2`..`xmm7`), and `[base±disp]`
displacement numbering. Everything else in the dump — including `r8`, `r9`,
staging registers, `stack_size`, and `preserve` lists — is compared
literally, so the pass pipeline below is written to reproduce the reference
shapes exactly on the checked-in tests while staying semantics-preserving on
arbitrary LowIR input.

## Ownership boundaries

- `dev/src/x86/mir_optimize.{h,cpp}` — the whole PA38 optimizer:
  `mir_optimize::OptimizeMirProgram(mir_model::MirProgram &, int level)`.
  Pure MIR-to-MIR; no LowIR or native knowledge.
- `dev/lowir2native.cpp` — threads the parsed `-O` level into the shared
  pipeline (parse → validate → lower → **optimize** → dump/encode).
- `dev/src/toolchain/compile_unit.cpp` — the cppgm++ shared LowIR-to-object
  tail calls the same `OptimizeMirProgram` after `LowerLowIRProgramToMir`,
  so driver-emitted objects/executables at `-O1/-O2` reuse the identical
  backend pipeline (the PA38 "not a display-only transform" rule).
- `dev/src/x86/mir_to_native.cpp` — `MI_RET` learns to accept a coalesced
  return register (`ret r8`/`ret rbx`): emit `mov rax, <reg>` before the
  epilogue when the operand is not already `rax`. `-O0` output is unchanged
  because baseline lowering always returns through `rax`.

## Register/def-use model

A small def/use classifier over `MirInstruction` drives every pass
(bitmask over 16 GPRs + 8 XMMs):

- Moves/loads/lea write their destination register; arithmetic
  (`add/sub/imul/and/or/xor/adc/sbb`, `neg/not/bswap`, `sext/zext`,
  `setcc`, shifts) reads+writes the destination.
- `mul/idiv/div/cqo` use/def `rax`/`rdx` implicitly; shifts read `rcx`;
  `copy_bytes`/`zero_bytes` use/def the `rsi/rdi/rcx/rax` string registers.
- Calls (`call`, `call *`, `tls_addr`) are barriers: they conservatively
  read every register (so defs feeding possible arguments survive) and
  clobber the caller-saved set. This matches the reference: `mov r8, 20`
  survives before `call @id`, and `lea rcx, [rbp-8]` survives when a call
  follows, while the same shapes are deleted in call-free tails.
- `ret` reads its operand; `exit` reads `rdi`; any unmodeled opcode
  (x87 `fstp`, `eh_landing`, atomics) is a full barrier and never deleted.

Liveness is a whole-function backward dataflow over the block CFG
(successors = label operands of `jcc/jmp`; `jmp *` conservatively targets
every block; blocks whose last instruction is not an unconditional
terminator fall through). Functions with host-EH regions are skipped
entirely: the unwinder re-enters landing pads with only `rbp` restored, the
conservative eh_mode lowering already keeps values in frame homes, and
implicit call→landing-pad edges would otherwise have to be modeled.

## `-O1` pipeline (block-local rewrites + global-liveness DCE)

Iterated to a fixpoint (propagation enables deletion enables propagation):

1. **Forward block-local propagation.** Tracks per-register facts, all
   invalidated on any write to the tracked or source register and at call
   barriers:
   - `lea R, [rbp±off]` → R holds a frame address; later `[R±d]` deref
     operands fold to direct `[rbp±(off+d)]` frame operands (frame-address
     fold; also applies to `lea`-staged call-argument setup).
   - `mov R, imm` → R holds an integer immediate; rematerialized into the
     source of a later `mov` (which covers call-argument staging copies)
     and the RHS of `add/sub/imul/and/or/xor` when it fits in imm32.
     Compares are deliberately not remat targets (`mov rdx, -1; cmp.i64
     r8, rdx` is the pinned reference shape), and no constant folding of
     branches is performed.
   - `mov R, S` / `fmov.T X, Y` → R/X holds a register copy; substituted
     into pure-read uses: deref bases, `mov`/`fmov` sources, float
     arithmetic/compare sources (same `.T` only), and the `ret` operand
     (return-shuffle coalescing). Never substituted into a read+write
     destination operand.
2. **Dead-copy elimination.** Deletes side-effect-free register writes
   (`mov`, `lea`, register/immediate `fmov`) whose destination is dead in
   the whole-function liveness. This removes the staging copies exposed by
   step 1 (`mov rax, r9; ret rax` → `ret r9` + deleted mov, dead `lea`
   temporaries, call-result copy chains) while keeping anything a later
   call might consume.
3. **Zero-compare rewrite.** `cmp.T R, 0` immediately followed by `jcc`
   becomes `test.T R, R` (identical flag results for every condition).
4. **Branch-tail collapse.** With the final block order: a trailing
   `jmp ^next-block` is deleted (fallthrough elision, including after
   `jcc` chains); otherwise `jcc ^next-block; jmp ^other` inverts the
   condition into `jcc' ^other` and drops the `jmp`.

Rewrites keep the consuming instruction's `!dbg` metadata and deletions
drop only the deleted instruction's location, so optimized debug metadata
stays valid (the reference `add r8, 2 !dbg(...)` shape).

## `-O2` additions

Between DCE and branch-tail collapse:

5. **Jump-trace layout.** Starting from the entry block, repeatedly place
   the current block's unique jump target next when it is not yet placed
   (only blocks with exactly one outgoing label are trace-followed, so
   conditional tails keep their source order); otherwise continue with the
   first unplaced block in original order. Runs only while every block
   still ends with an explicit unconditional terminator. The following
   collapse pass then turns the new adjacency into fallthrough.

After collapse:

6. **Callee-saved prune.** Drop `preserve` registers no longer mentioned
   by any surviving instruction (prologue/epilogue save-restores and the
   EH saved-reg table derive from this list).
7. **Stack recompute.** Callee-saved save slots live inside `stack_size`
   (8 bytes each, above `scratch_bytes`); pruning k registers frees 8k
   bytes, reclaimed in 16-byte alignment steps
   (`stack_size -= 16*floor(8k/16)`). The baseline frame policy (minimum
   reservations, slot layout) is deliberately preserved — the reference
   keeps `stack_size 16` for leaf functions with empty frames at `-O2` —
   so the recompute only reclaims what optimization actually freed.

## Validation

- `make -C pa38 check TEST=...` per fixture, then `make test-pa38`.
- Root `make test-report-through-pa38` as the exit gate: PA28 `-O0` strict
  fixtures pin that the baseline is untouched; PA37 object-roundtrip pins
  that source-path and LowIR-path objects stay byte-identical (both go
  through the one shared tail in `compile_unit.cpp`); the PA38 generated
  programs pin behavior preservation.
- Debuginfo lanes (`pa38/tests/debuginfo/*`): the optimizer preserves
  `MirInstruction::debug_location` by construction. The LowIR `!dbg` →
  MIR lowering plumbing and `!dbg` dump serialization remain untracked
  (same status PA37 left them); wiring them does not change pass logic.
