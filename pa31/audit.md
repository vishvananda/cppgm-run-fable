# PA31 Audit

## Audit Plan

Scope: commits a82917e5a..bfd0a3ae6 (host EH objects) against `pa31/plan.md`
and `pa31/README.md`, plus the regression surface of the earlier stages the
change touched (pa13 LowIR, pa24-pa28 backend, pa25/pa29 private-link EH).

### Files to inspect

- `dev/src/x86/lowir_to_mir_eh.cpp` — region dataflow, landing-pad
  synthesis, throw-window regions, action-chain construction.
- `dev/src/x86/lowir_to_mir_flow.cpp`, `lowir_to_mir_program.cpp`,
  `lowir_to_mir.h` — integration of the EH pass with the existing lowering.
- `dev/src/x86/mir_to_native.cpp`/`.h`, `mir_serialize.cpp`,
  `dev/src/mir_model.h` — offset recording, frame facts, landing-pad entry
  code, the 9..16-byte return fix.
- `dev/src/x86/frame_cfi.cpp` — CFI program generation.
- `dev/src/toolchain/eh_table.cpp` — LSDA encode/decode round-trip.
- `dev/src/toolchain/elf_object.cpp` — ET_REL writer (sections, symbols,
  relocations, `.eh_frame`, `.gcc_except_table`).
- `dev/src/toolchain/elf_reader.cpp` — role + EH fact recovery, foreign-FDE
  skipping.
- `dev/src/toolchain/object_module.cpp`/`.h`, `compile_unit.cpp` — typed EH
  structs, typeinfo demotion, CPGMOBJ1 removal.
- `dev/src/toolchain/link_executable.cpp`, `runtime_library.cpp`/`.h` —
  flat region table, `__cppgm_get_frame`/`__cppgm_land` primitives, rbp-chain
  walker, runtime-owned fundamental typeinfo.

### Named risks to check

- Plan's own open item: register allocator double-books a callee-saved pool
  register (parameter copied to pool + negation temp); plan says it was
  side-stepped via `cppgm_eh_record_of` and deferred. Must reproduce/fix or
  prove absent now.
- Cheating vectors: fallback success paths in `-c`; test-shape gates keyed on
  test names/source text; LSDA/CFI facts derived from source text instead of
  machine layout; ELF reader "recovery" that silently drops facts; private
  walker substituting for real host CFI (host tests must actually exercise
  `.eh_frame`).
- Regression risk: pa25/pa26/pa27 pinned LowIR unchanged; pa28 MIR fixtures
  (register discipline pinned per pa28/plan.md); pa29 object round-trip now
  going through ELF instead of CPGMOBJ1; pa24+ execution tests.
- Ownership/stringly risks: runtime-role classification by string name
  matching; `IsRuntimeOwnedTypeinfoName` as single owner of the demotion set;
  CIE "emission fingerprint" matching in the reader duplicated from the
  writer; action-chain/ttype knowledge split between eh_table and elf_object.
- Performance risks: per-call region lookup (quadratic scans over regions or
  blocks), full re-walks of module lists during flat-table synthesis, LSDA
  decode over every FDE of every loaded object, byte-vector copying in the
  ELF writer.
- File-audit risks: new files near the size limit, code moved to unchecked
  paths, hidden fragments.

### Checks to run

- `perl scripts/cppgm_file_audit.pl --stage pa31 --paths dev/src`
- `make test-report-through-pa31`
- Targeted spot checks: `readelf -wf` on a generated object, LSDA
  encode/decode round-trip inspection, allocator repro for the double-booking
  bug.

## Findings

### Blockers (fixed this turn)

1. **Private-path unwinder corrupted ancestors' callee-saved registers**
   (`dev/src/x86/lowir_to_mir_program.cpp`, `FinishFrame`). The flat-table
   walker lands via `__cppgm_land` installing only rax/rdx/rbp; the
   callee-saved spills of the frames between the throw and the landing pad
   are abandoned with those frames. A catch frame that did not itself touch
   a pool register (eh_mode functions allocate none) therefore returned to
   its caller with whatever a deeper, abandoned frame had left in
   rbx/r12-r15. Reproduced with a program where `main` holds a temp in rbx
   across a call to a catcher while the thrower's callee clobbers rbx:
   private-link exit was 129 instead of 71 (host-linked exit was correct,
   because host CFI restores the register). This was a latent hazard of the
   old handler-chain engine too, but PA31 re-implemented the engine, so it
   is PA31's to fix.

2. **Register allocator double-booked a pool register for hoisted parameter
   copies** (`dev/src/x86/lowir_to_mir_value.cpp`, `resolve_location`).
   plan.md deferred this as "worth a dedicated look in a later audit turn" -
   this is that turn. A scratch parameter's copy is materialized at its
   first read but hoisted into the prologue; the target register was chosen
   from the currently-free pool without checking whether already-emitted
   code had used (and released) it. Reproduced with the old `__cxa_throw`
   shape: `dtor` was copied to rbx in the prologue while a `0 - 64`
   negation temp had already been assigned rbx mid-function, so
   `record->dtor` stored -64. A live miscompile of ordinary (non-EH) code,
   not just an EH-runtime curiosity.

3. **Quadratic block-label scans in the always-on EH dataflow**
   (`dev/src/x86/lowir_to_mir_eh.cpp`). `MergeEhBlockState` and
   `CollectEhClauses` resolved labels by linear scan over all blocks;
   `AnalyzeEhRegions` runs for every function since PA31 (every stage >=
   pa24 pays it), so terminator-heavy functions scanned O(blocks) per edge,
   O(blocks^2) per function.

4. **`LowerIndex` pinned-base fast path dropped runtime element counts**
   (`dev/src/x86/lowir_to_mir_inst.cpp`). The callee-saved-base fast path
   called `ParseIntLiteral` on the count operand without checking it was a
   literal; a temp count parsed as 0 and the emitted `lea reg,[reg+0]`
   silently discarded the index. Not reachable from the C++ frontend
   (promoted slot loads never span calls, so frontend index bases are not
   pinned callee-saved with temp counts), but LowIR is the public input
   format for pa13-pa28 stages: a hand-written `index` on a cross-block
   base with a temp count returned arr[0]+arr[0]=20 instead of
   arr[3]+arr[0]=50. Surfaced by the double-booking investigation.

### Checked and clean

- **Cheating vectors**: a very-thorough sweep of `dev/src` and
  `dev/cppgm++.cpp` found no test-name/path/env gates, no fallback success
  paths (every phase failure propagates to `EXIT_FAILURE`; no dummy object
  emission), no embedded binary payloads or reference-binary copying, no
  interpreter/VM/trampoline substitutes (output is genuinely encoded
  x86-64 + hand-assembled ELF), no timing workarounds, and no code hidden
  from `cppgm_file_audit.pl` (no non-audited file types under dev/src; the
  two exempt files are the test harness and a help string, neither on the
  compile path).
- **Compiler pipeline**: `cppgm++ -c` runs preprocess -> parse -> bind ->
  lower -> LowIR validate -> MIR -> native encode -> ELF write; the ELF
  writer builds real CIEs/FDEs/LSDAs with real relocations (verified
  against `readelf -wf`), and the reader recovers typed facts only from
  our documented CIE fingerprint, skipping foreign FDEs.
- **LSDA codec round-trip**: encoder and decoder live in one file
  (`eh_table.cpp`) and are exercised end-to-end by the pa29 object
  round-trip (ELF re-read) plus the pa31 fact decoder. The one-byte sleb
  action profile (>63 distinct catch filters) throws loudly rather than
  emitting bad tables - an acceptable, explicit subset boundary.
- **Ownership/stringly facts**: runtime-role classification rides LowIR
  metadata (typed, PA30-owned); `IsRuntimeOwnedTypeinfoName` is the single
  owner of the fundamental-typeinfo set; host format is the boundary
  encoding with typed `EhFunctionInfo` as source of truth on both sides.
- **Regression surface**: pa25/26/27 pinned LowIR untouched; pa28 strict
  MIR fixtures pass bit-identical; pa13 LowIR EH semantics pass through
  the new engine; pa29 object round-trip passes through ELF.
- **Performance elsewhere**: flat-table synthesis, reader recovery, and
  ELF emission are linear in their inputs; no repeated full-suite walks or
  hot-path recomputation found.

## Changes Made

1. `dev/src/x86/lowir_to_mir_program.cpp` (`FinishFrame`): functions owning
   a real landing pad (non-synthetic region targets) now save the full
   callee-saved pool set (rbx, r12-r15) in the prologue. The landed frame's
   epilogue restores its entry snapshot, which is exactly the state its
   ancestors need regardless of what abandoned deeper frames did; the host
   unwinder gets the same snapshot via the CFI offset rules. Throw-only
   and non-EH functions are unaffected.
2. `dev/src/x86/lowir_to_mir_value.cpp` (+ `lowir_to_mir.h`,
   `lowir_to_mir_analyze.cpp`, `lowir_to_mir_flow.cpp`,
   `lowir_to_mir_inst.cpp`): added `pool_clobbered_` tracking, set at every
   booking that actually writes a pool register (pending deferred-load
   reservations excluded - they reserve without writing). The hoisted
   prologue copy in `resolve_location` now only targets never-clobbered
   registers and otherwise falls back to the existing named-frame-home
   path.
3. `dev/src/x86/lowir_to_mir_eh.cpp` (+ `lowir_to_mir.h`): the region
   dataflow builds a label -> block-index map once per function;
   `MergeEhBlockState` and `CollectEhClauses` use it instead of scanning
   the block list.
4. `dev/src/x86/lowir_to_mir_inst.cpp` (`LowerIndex`): the pinned-base
   fast path now requires a literal count; runtime counts fall through to
   the general scale-in-rdx path, which already handles pinned bases.
5. `pa31/plan.md`: replaced the deferred allocator note with the fix
   description; added Architecture Review and Final Architecture Review.

## Validation

- Repro 1 (callee-save corruption): private-link exit 129 -> 71 after the
  fix; host-link stays 71; the pre-existing simpler variant (frame-homed
  locals) stays 71.
- Repro 2 (double-booking): `record->dtor` now stores the parameter (the
  hoisted copy targets virgin r12; the negation temp keeps rbx); the MIR
  dump shows disjoint registers.
- Repro 3 (index fast path): the hand-written LowIR case exits 50 after
  the fix (was 20), and the MIR shows the scale-in-rdx sequence instead of
  the degenerate `lea reg,[reg]`.
- `readelf -wf` on a generated object: both CIEs and all FDEs parse; catch
  frames carry the full rbx/r12-r15 offset rules.
- `make -C pa13/pa25/pa28/pa29/pa31 test`: all pass, including pa28's
  34/34 strict register-discipline fixtures (bit-identical - the allocator
  fix only changes previously-miscompiled shapes).
- Required exit criteria:
  - `perl scripts/cppgm_file_audit.pl --stage pa31 --paths dev/src`:
    passed (6 pre-existing header-division warnings, no fatals).
  - `make test-report-through-pa31`: ALL TESTS PASSED (2765/2765), same
    totals as the pre-audit baseline run.
