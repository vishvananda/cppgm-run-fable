# PA28 Audit

## Audit Plan

Scope: the five PA28 commits `4bc22187e..02d36aad4` — the new
`dev/src/x86/lowir_to_mir*` lowering stack, `mir_serialize.cpp`,
`mir_to_native.cpp`, the extended `x86_encoding.*`, the shared
`dev/src/lowir/*` edits, and the `dev/lowir2native.cpp` driver.

Files to inspect:

- `dev/src/lowir/lowir_lexer.cpp`, `lowir_parser.cpp`,
  `lowir_validate.*`, `lowir_program.h` — shared with PA13-PA27;
  verify every change is additive (signed exponents, hex-float
  classification, bare span counts, atomic-order capture, opt-in
  `require_entry`) and cannot change accepted/rejected behavior for
  earlier stages.
- `dev/lowir2native.cpp` — driver surface: no fallback success paths,
  no NotImplemented shortcuts, errors exit failure.
- `dev/src/x86/lowir_to_mir.h` + `lowir_to_mir_analyze.cpp` +
  `lowir_to_mir_value.cpp` + `lowir_to_mir_inst.cpp` +
  `lowir_to_mir_flow.cpp` + `lowir_to_mir_program.cpp` — the lowering
  core. Verify the analyze/value split (commit `1d355ed0d`, made "to
  satisfy the file audit") is a real module boundary, not a hidden
  fragment; verify no test-shape or fixture-name gates; check register
  discipline is driven by typed facts, not serialized-string sniffing.
- `dev/src/x86/mir_serialize.cpp` — dump must be a pure serialization
  of the same `MirProgram` the encoder consumes; no dump-only
  rewriting that would make the dump lie about the encoded program.
- `dev/src/x86/mir_to_native.cpp`, `x86_encoding.*`, `elf_program.*`
  — must be a genuine MIR-to-x86 encoder: no interpreter, VM,
  trampoline into host libc, templated binary, or embedded
  earlier-IR/CY86 payload; atomics must emit real lock-prefixed
  encodings; `copy_bytes`/`zero_bytes` real loops.

Performance risks to inspect:

- per-instruction rescans of whole functions (liveness/next-use,
  compare sinking, dead-copy sweep, preserve computation);
- repeated `std::map<string,...>` lookups on hot per-instruction
  paths; excessive value copies of `MirFunction`/blocks;
- serializer building output via repeated string concatenation over
  large programs;
- encoder label patching that rescans the image per label.

Ownership boundaries to inspect:

- backend facts must come from LowIR text or the typed MIR — no
  source-level or semantic backchannel (README Assignment Boundary);
- atomic memory orders must be typed on `LowIRInstruction`, not
  re-parsed downstream;
- TLS wrapper resolution must flow through the MIR
  (`tls_wrappers`), not be re-derived by symbol-name string sniffing
  in the encoder;
- `result_copy_hint` cleanup must happen in lowering, before both the
  dump and the encoder consume the MIR.

File-audit issues to inspect:

- `perl scripts/cppgm_file_audit.pl --stage pa28 --paths dev/src`
  must pass with no new warnings introduced by PA28 files;
- no implementation moved into headers or non-audited paths to dodge
  size checks; the `lowir_to_mir_analyze`/`lowir_to_mir_value` split
  must leave each file with a describable single responsibility;
- pa28 harness files (`pa28/scripts`, `pa28/tests`, `pa28/Makefile`,
  `pa28.gram`) must be unmodified — confirmed via git log before
  writing this plan: only `pa28/plan.md` was added.

## Findings

Four independent detailed reviews covered the encoder
(`mir_to_native.cpp`, `x86_encoding.*`, `elf_program.*`), the lowering
core (`lowir_to_mir_flow|inst|analyze|value|program.cpp`), the
serializer/driver, and the shared PA13 front-half diffs.

Confirmed clean (no action needed):

- **Genuine backend.** Every MIR instruction is encoded operand-by-
  operand through the PA9 encoder into a hand-built ELF image. No
  interpreter, VM, trampoline, templated binary, embedded payload, or
  CY86 anywhere in the path. Atomics emit real `lock`-prefixed
  encodings at all widths; `copy_bytes`/`zero_bytes` are real
  `rep movsb`/`rep stosb`; f80 is genuine x87 with correct rounding-
  control handling; unsigned conversions use the branch-and-adjust
  sequences. Unknown opcodes/types/operand shapes throw.
- **No test-specific gates.** No symbol-name, fixture-name, label, or
  instruction-count branching anywhere in `dev/src/x86`. The corpus-
  pinned frame-residue rules are shape predicates, not name tables.
- **Dump purity.** `RemoveDeadResultCopies` runs inside lowering, so
  the `--dump-machine-ir` output and the encoder consume the identical
  post-cleanup `MirProgram`; the serializer never filters or reorders.
- **Shared-file regressions.** Every `dev/src/lowir` change is strictly
  rejected-input -> accepted-input (proven at grammar level and by
  corpus scans); new `LowIRInstruction` fields default to the previous
  behavior (order 5 = seq_cst); PA13's driver keeps `require_entry`.
- **u-predicates on floats** alias to the ordered forms — identical to
  PA13's cy86 backend ("unsigned predicate spellings compare the same
  way on floats"), so this is project-wide LowIR semantics, not a
  PA28 shortcut.
- **Call-through-cell def-sniffing** (`call %t` where `%t = addr
  @data-global` loads the stored pointer) implements README goal 4's
  pointer-cell rule from typed facts (def opcode + operand kinds).

Defects found and fixed (all were silent-wrong-code paths reachable
only outside the fixture corpus; the shipped tests never trigger them):

1. **Dropped call-crossing parameter** (blocker). `PlanParams`' reverse
   callee-saved assignment did `continue` on pool exhaustion, leaving
   the parameter with no `ValueLocation`; `emit_dest_copy` then read
   the default location's `rax`. Six i64 params live across a call
   miscompiled silently (verified: returned 62 instead of 63).
2. **Deferred load feeding a stack call argument** (blocker).
   `try_defer_load` accepted `USE_CALL_ARG` without knowing the ABI
   slot, but only GPR staging consumed pending loads; a 7th integer
   argument fed by a single-use load stored garbage from `rax`.
3. **No XMM parallel-move at calls** (blocker). Float argument staging
   wrote `xmm0..k` in order with only an in-place skip; a source
   living in a lower xmm than its slot was clobbered before being
   read (two swapped f64 args both arrived as the first value).
4. **By-value `obj>8` arguments truncated** (blocker, both sides).
   Caller staging stored 8 bytes (or an address) into the reserved
   stack region; `PlanWideParam` copied only 8 bytes of the incoming
   stack object into its local home.
5. **Forwarded-param bounce skipped on pool exhaustion** (minor).
   Argument staging left a forwarded value in its incoming register
   when no scratch register was free; now parks it in the frame.
6. **Call-arg ABI classification owned three times** (structural
   enabler of #5). `classify_args` (staging), `CallArgTargetsHome`
   (forwarding), and the analyze-side copy classified variadic float
   tails differently (I64 vs value type), so forwarding could target
   a different register than staging wrote. `ClassifyCallArgs` is now
   the single owner consumed by both.
7. **Float `unary` lowered any operation as negation** (minor); now
   throws for anything but `neg`.
8. **Unknown global-init forms zero-initialized silently** (minor);
   now throws (declaration-only globals are filtered by the caller).
9. **`TypeBits` defaulted unknown spellings to 64 bits** (minor); now
   an explicit whitelist that throws on producer bugs.
10. **Unchecked compare/test/prologue immediates** (defense).
    `EmitCompare`/`EmitPrologue` truncated imm32 silently if the
    producer's `FitsImm32` invariant ever broke; now `CheckedImm32`.

Ownership/duplication cleanups:

- `kArgRegs` (3 copies), pool order (4 copies plus an enum-arithmetic
  re-derivation in the TLS store path), `kCalleeSavedStart` literals,
  `ContainerSpelling`, `param_wants_address`/`param_expects_address`,
  `FitsImm32`, TLS/embedded-call predicates (`_probe` twins), and the
  callee-param lookup (3 copies) each collapsed to one owner in
  `lowir_to_mir_model.h` / `lowir_to_mir_value.cpp` /
  `lowir_to_mir_analyze.cpp`.
- Dead code removed: write-only `preserve_` vector and
  `note_callee_saved` (11 call sites; `FinishFrame` recomputes the
  preserve set from the final body, which is the correct owner), the
  never-written `"*hole:"` pool sentinel comparison, a no-op
  self-assignment in `ClassifyCallArgs` with a misleading comment.
- `MarkByAddressArgs` (an analysis pass) moved from program.cpp to
  lowir_to_mir_analyze.cpp with the other analysis passes.
- Obj stack-argument size rule now computed once in `ClassifyCallArgs`
  (`ArgSlot::stack_bytes`) instead of re-derived in `stack_bytes_of`.

Performance fixes (all previously O(n^2)-class on large functions):

- `PromoteSlots`: one linear pass with per-slot summaries plus a
  call-position prefix count, replacing a whole-function rescan per
  slot and a per-slot store-to-load call scan.
- `AliasObjectParamSlots`: temp-reference counts computed once,
  replacing a whole-function rescan per candidate copy.
- `MarkCallCrossings`: embedded-call flags computed once per call and
  a binary search over sorted call positions, replacing an
  O(values x calls) loop that re-evaluated `instruction_embeds_call`
  per pair; the per-value block scan now binary-searches.
- `fused_compare_for_branch`/`try_defer_load`: enclosing-block bounds
  via `std::upper_bound` on `block_first_position_` instead of a
  linear block-table walk per invocation.

File-audit posture:

- The `lowir_to_mir_analyze/value/program` split (commit `1d355ed0d`)
  is a real module boundary (analysis passes / value machinery /
  scaffolding); no `.cpp` includes another, and the extracted helpers
  are reused, not hidden fragments.
- The shared lowering data model (value facts, locations, argument
  classification, register constants) moved into
  `lowir_to_mir_model.h`, mirroring the existing `mir_model.h` /
  `lowir_model.h` pattern; this also keeps `lowir_to_mir.h` under the
  audit's header-weight threshold with zero PA28 warnings.

Reviewed and accepted as-is:

- The `exit(0)` startup sled for entry-less `-o` builds is the
  documented helper-only contract (plan.md "Native encoding"); an
  upstream regression producing an empty startup for an entry-ful
  program would fail every program-exit-status oracle immediately.
- The dump omits `tls_wrappers` (recoverable from the wrapper
  functions; the dump format is fixture-pinned).
- The serializer re-derives the abi `return -> rax|xmm0|st0` line from
  the return type while the encoder bakes the convention into
  `MI_RET`/`MI_FRET`; consistent today, noted as a two-place edit if
  the return convention ever changes.
- `resolve_location` materializes lazy parameter copies (emits into
  the entry prologue) despite its getter-like name; this is the
  documented lazy-copy design (plan.md "Register discipline") and is
  commented at the definition.

## Changes Made

- `dev/src/x86/lowir_to_mir_program.cpp` — crossing-loop pool
  exhaustion now falls back to `SpillParamHome` (fix #1);
  `PlanWideParam` copies the full padded container for `obj>8` stack
  parameters (fix #4 callee side); unknown global-init forms throw
  (fix #8); duplicated helpers removed.
- `dev/src/x86/lowir_to_mir_flow.cpp` — pending loads consumed by the
  integer stack-argument path (fix #2); XMM pre-staging spill loop
  (fix #3); by-value `obj>8` stack arguments copied in 8-byte chunks
  (fix #4 caller side); forwarded-param bounce parks in the frame on
  pool exhaustion (fix #5); `ClassifyCallArgs` shared and
  `stack_bytes` owned by classification; no-op self-assignment
  removed.
- `dev/src/x86/lowir_to_mir_analyze.cpp` — shared `StorageIsTls`/
  `InstructionEmbedsCall`/`FindCalleeParams`; `CallArgTargetsHome`
  rewritten over `ClassifyCallArgs` (fix #6); single-pass
  `PromoteSlots`; precomputed reference counts in
  `AliasObjectParamSlots`; binary-search `MarkCallCrossings`;
  `enclosing_block_begin/end`; `MarkByAddressArgs` moved here.
- `dev/src/x86/lowir_to_mir_inst.cpp` — float unary guard (fix #7);
  `kPool` used everywhere (including the TLS store staging that
  re-derived registers by enum arithmetic); dead `"*hole:"` sweep
  removed; block lookups via the shared helpers.
- `dev/src/x86/lowir_to_mir_value.cpp` — single definitions of the
  register constants and shared fact helpers; `note_callee_saved`
  and `preserve_` removed.
- `dev/src/x86/lowir_to_mir_model.h` (new) / `lowir_to_mir.h` — data
  model split from the engine class declaration.
- `dev/src/x86/mir_to_native.cpp` — `TypeBits` whitelist (fix #9);
  `CheckedImm32` on compare/test immediates and the prologue `sub
  rsp` (fix #10).

## Validation

- `pa28: make test` — strict 34/34, structural 58/58, behavior 14/14
  after every change cluster (the strict suite pins the register
  discipline, so it gates every refactor above).
- End-to-end reproductions, before -> after:
  - six call-crossing i64 params: exit 62 (wrong) -> 63, with the
    sixth parameter visible as a `param-slot` + entry store in MIR;
  - 7-argument call with a single-use load feeding the stack arg:
    exit 77 (correct value now loaded into r11 before the store);
  - swapped f64 arguments `fsub(%a, %b)` staged from xmm1/xmm0: exit
    8 (10-2) with the endangered source spilled to `[rbp-8]` and
    reloaded into its slot;
  - by-value `obj<16x8>` argument through a slot: exit 42 with both
    8-byte chunks copied on the caller and callee sides;
  - helper-only input: `--dump-machine-ir` exits 0 with no `startup`
    section; parse errors exit 1.
- `perl scripts/cppgm_file_audit.pl --stage pa28 --paths dev/src`:
  exit 0, zero warnings on PA28 files.
- `make test-report-through-pa28`: all stages pass (see plan.md
  Validation; PA13-PA27 confirm the shared LowIR front-half changes
  and the x86 encoder extensions regress nothing).
