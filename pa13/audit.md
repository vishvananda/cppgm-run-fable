# PA13 Audit: `lowir2cy86`

## Audit Plan

Scope: commit 3f42fba25 (PA13 implementation) against `pa13/README.md`,
`pa13/pa13.gram`, `pa13/lowir.md`, and `pa13/plan.md`.

Files to inspect:

- `dev/lowir2cy86.cpp` — driver surface, exit-status policy, no fallback
  success paths.
- `dev/src/lowir/lowir_lexer.{h,cpp}` — token coverage vs `pa13.gram`
  terminals (sigil names, `NxA` spans, `obj<NxA>`, `!dbg(...)`).
- `dev/src/lowir/lowir_parser.{h,cpp}` — production coverage vs
  `pa13.gram`; statement/rvalue split; line-sensitive `return` value.
- `dev/src/lowir/lowir_program.{h,cpp}` — type model ownership; size and
  width helpers as the single source of truth for validator and emitter.
- `dev/src/lowir/lowir_validate.{h,cpp}` — every rejection in the README
  "Structural Validation" list; metadata tables vs `lowir.md`; hook
  resolution ownership.
- `dev/src/lowir/lowir_frame.{h,cpp}` — frame layout vs the emission
  contract in `plan.md`; scratch-tail detection completeness.
- `dev/src/lowir/lowir_to_cy86.h`, `lowir_emit_*.cpp` — template
  determinism, ref conformance, latent miscompiles on in-contract inputs
  not pinned by a checked-in ref.
- `dev/frontend_source_sets.mk`, `pa13/Makefile`, harness scripts — build
  wiring, no reference-binary consumption, no test-specific gates.

Performance risks to check:

- per-instruction emission allocating or rescanning program-wide state;
- metadata lookups and symbol resolution on hot paths;
- repeated whole-program walks (EH detection, hook resolution);
- string accumulation behavior in the emitter.

Ownership boundaries to check:

- driver stays thin (argv, file IO, exit status);
- validator owns legality and hook resolution; emitter consumes resolved
  `LowIRProgramInfo` facts instead of re-deriving them;
- `LowIRType` helpers own size/width facts once;
- no `dev/src/cy86/` reuse, no shelling out, no reference binaries.

File-audit issues to check:

- `perl scripts/cppgm_file_audit.pl --stage pa13 --paths dev/src` clean;
- no implementation hidden outside audited paths; no oversized files.

Cheating patterns to check:

- output keyed to test names or source shapes;
- embedded CY86 payloads or copied `.ref` content;
- interpreter/VM/trampoline substitutes instead of translation;
- validator accepting everything and relying on EXIT_FAILURE tests being
  few; emitter emitting dummy text for unhandled instructions.

## Findings

1. **Latent miscompile: f80 literal call arguments had no storage.**
   `emit_call` materialized an f80 literal argument into
   `frame.scratch(1)`, but `instruction_needs_scratch` never flagged call
   instructions whose only f80 mention is a literal operand (the call's
   own type and signature need not mention f80 when the callee's declared
   parameter or a variadic tail receives the literal). In such a function
   the scratch tail was never allocated, so the `move80` landed below
   `sp`, exactly where the subsequent `call` pushes the return address
   and saved `bp` — the argument bytes were clobbered before the callee
   could read them. Additionally, when one call passed two or more f80
   literals, every literal shared `scratch(1)`: each materialization
   overwrote the previous one, so all such argument pointers aliased the
   last literal's value. No checked-in ref exercises f80 literal call
   arguments, so the suite could not catch either defect; both are
   in-contract inputs (calls + f80 are both required PA13 features).
   Fixed in `lowir_frame.cpp`/`lowir_emit_inst.cpp` by giving every f80
   literal call argument its own 16-byte frame home (see Changes Made).
   By-address literal arguments with a non-f80 spelling (type-mismatched
   garbage such as an integer literal against a declared `obj` parameter)
   now fail translation instead of silently emitting a `move80` of a
   non-f80 spelling.

2. **Oracle-mandated x64 intermediary in call setup (not a defect we may
   change).** By-address arguments in units past 0 route their address
   through `x64` before moving it to the unit register, which clobbers a
   unit-0 value already living in `x64` (visible in
   `200-f80-direct-call.ref`: the hidden result pointer in `x64` is
   overwritten by the unit-1 argument address before `call fn__id`). The
   checked-in ref encodes exactly this sequence, and PA13 grades exact
   text equality, so the adapter must reproduce it. Documented here so a
   later assignment that executes generated CY86 does not mistake the
   inherited template for a regression of ours.

3. **No skipped phases, dummy outputs, or acceptance gates.** The driver
   always runs lex → parse → validate → translate and only writes the
   outfile on full success; failures exit `EXIT_FAILURE` via exceptions.
   There is no test-name, path, or source-shape inspection anywhere in
   `dev/src/lowir/`. No embedded CY86 payloads: every output line is
   constructed from the parsed program. `pa13/lowir2cy86-ref` is the
   starter kit's optional harness symlink; nothing in the build or the
   tool consults it.

4. **Performance clean.** Lex, parse, validate, frame layout, and
   emission are all single-pass and linear in program size; symbol
   resolution uses maps/sets; the EH-usage scan is one pass that stops at
   the first hit; emission appends to one `string`. Metadata lookups are
   linear over per-item lists that hold at most a handful of entries.

5. **Ownership boundaries hold.** The driver is 96 lines of argv/IO
   policy. The validator alone owns legality tables and entry/init/fini
   hook resolution; the emitter consumes `LowIRProgramInfo` instead of
   re-scanning metadata. Type size/width facts live only on `LowIRType`.
   Semantic facts that matter to PA13 (types, opcodes, operand kinds,
   literal classes) are modeled as enums; metadata stays textual
   key/value pairs because the LowIR contract itself is textual metadata
   that PA13 validates and otherwise preserves.

6. **File audit clean.** `--stage pa13 --paths dev/src` passes; the one
   warning (`dev/src/parse/parser.h` header body) predates PA13 and
   belongs to the PA10 parser family, unchanged by this assignment.

## Changes Made

- `dev/src/lowir/lowir_frame.h` — added
  `LowIRFrame::call_literal_homes`, 16-byte homes for f80 literal call
  arguments in block/instruction/operand order.
- `dev/src/lowir/lowir_frame.cpp` — the existing single instruction walk
  now allocates one home per f80 literal call argument (before the call's
  own result home, deterministically).
- `dev/src/lowir/lowir_to_cy86.h` — added the per-function
  `call_literal_index` cursor.
- `dev/src/lowir/lowir_emit_program.cpp` — `emit_function` resets the
  cursor alongside the frame.
- `dev/src/lowir/lowir_emit_inst.cpp` — `emit_call` materializes each
  by-address f80 literal into its own frame home instead of a shared
  scratch slot, and rejects by-address literals whose spelling is not
  f80 instead of emitting a malformed `move80`.

No other code changes were warranted: the remaining templates match the
checked-in refs exactly, and the suite pins them.

## Validation

- `make -C pa13 test` — full local suite passes (90 tests).
- New translation exercised manually: an f80 literal passed to a declared
  f80 parameter in a function with no other f80 mention now allocates a
  16-byte home inside the frame (not below `sp`), and two f80 literals in
  one call get distinct homes; outputs for every checked-in test are
  byte-identical to before the change.
- `make test-report-through-pa13` — all stages pass.
- `perl scripts/cppgm_file_audit.pl --stage pa13 --paths dev/src` —
  passes (pre-existing PA10 warning only).
