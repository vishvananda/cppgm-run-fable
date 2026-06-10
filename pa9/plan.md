# PA9 (cy86) Plan

## Goal

`cy86 -o <outfile> <srcfile1> ... <srcfileN>` translates CY86 Mock
Intermediate Language sources into a native Linux x86-64 ELF
executable. Phases 1-6 plus the tokenization part of phase 7 are the
PA5 pipeline unchanged; the per-TU phase-7 token sequences are
concatenated in command-line order and matched against `pa9.gram`.
Diagnosable ill-formed programs exit EXIT_FAILURE; the generated
program's stdout and exit status are what the tests compare.

## Pipeline and ownership boundaries

    Preprocessor + PostTokenizer (existing dev/src, per srcfile)
       |  vector<PostToken> (PA2 typed literals are the value oracle)
       v
    CY86Parser            dev/src/cy86/cy86_parser.{h,cpp}
       |  CY86Program: statements with typed operands + label table
       v
    CY86ToX86Translator   dev/src/cy86/cy86_codegen.{h,cpp}
       |  X86Instruction sequences + data payloads per statement
       v
    X86Assembler          dev/src/x86/x86_encoding.{h,cpp}
       |  machine code bytes + symbolic immediate patches
       v
    ProgramImage/ELF      dev/src/x86/elf_program.{h,cpp}

- `cy86_parser` owns the CY86 object model: the opcode table
  (transcribed from `cy86-opcode.desc` as typed constraint data:
  read/write/address/bool/signed/unsigned/float/immediate-only flags
  plus bit width), operand variants (register, immediate
  literal/label+-offset, memory [reg|label|literal +- offset]), and
  all front-end diagnoses: keywords and user-defined literals are
  errors, label spellings may not collide with opcodes, registers, or
  other labels, opcode arity/kind/width constraints, memory bases
  must be 64-bit registers, label+-literal offsets must be integral.
  Literal immediates are width-converted at parse time from the PA2
  value bytes (truncate rightmost bytes; sign-extend signed integral,
  else zero-extend); label immediates stay symbolic (label id +
  64-bit addend) until layout.
- `cy86_codegen` maps each CY86 instruction to a fixed x86 sequence.
  Register backing: sp->rsp, bp->rbp, x->r12, y->r13, z->r14,
  t->r15. Scratch: rax/rbx/rcx/rdx for data, rsi/rdi for addresses,
  red zone [rsp-8..rsp-128] for syscall argument staging and x87
  spills. Integer ops use width-exact x86 forms; floats go through
  the x87 stack (fld/fild operands, faddp/fsubp/fmulp/fdivp/fcomip,
  fstp/fistp results); u64<->f80 use the 2^63/2^64 adjustment idiom.
  Memory operands encode directly as absolute [disp32] or
  [base + disp32] addressing modes when the address term allows;
  only label-plus-base sums compute addresses in rsi/rdi. Control
  transfers to immediate targets use jmp/call/jcc rel32; computed
  targets go through a register.
- `x86_encoding` owns the X86Instruction object model (mnemonic +
  Reg/Mem/Imm operands) and the generic encoder (legacy prefixes,
  REX, opcode, ModRM, SIB, disp, imm). Immediates carry an optional
  symbolic label reference; every label-bearing form encodes with a
  value-independent length (mov r64, imm64, absolute [disp32]
  operands, rel32 transfers, fixed-width data), so statement sizes
  are final on the first pass. Constant immediates pick the shortest
  64-bit-faithful mov form (imm32 zero-extend, imm32 sign-extend for
  [-2^31, -1], else imm64). Patches carry a kind: data fields
  truncate, absolute and pc-relative code fields are range-checked
  at layout against the CPU's sign-extension of the field.
- `elf_program` owns layout and output: items are code bytes or data
  blobs with alignment (literal statements pad to their PA2 type
  alignment, dataN to N/8); offsets are assigned sequentially, labels
  bind to their statement's post-padding address at 0x400000 + 120 +
  offset, recorded patches are applied, and the ELF header + one RWX
  PT_LOAD segment + payload is written and chmod 0755. Statement
  addresses are contractual (fixtures compute sizes as label
  differences, for example `isub64 x64 start data`), so layout never
  inserts padding between items beyond the documented natural
  alignment.

## Code/data cache-line isolation

A store into a cache line that also holds executed (or
prefetched-ahead) instructions triggers the CPU's self-modifying-code
machine clear, a full pipeline flush. CY86 programs declare hot
store-target data directly adjacent to the code that loops over it
(`300-binary-calculator` spent 8.7 of its 8.8 seconds on these
clears; the harness allows 10 before EXIT_TIMEOUT). Because item
addresses are contractual, the isolation lives inside the code
items, whose translation sizes are implementation-defined:

- a code item that follows data keeps its label address but starts
  with a jmp-rel32 entry sled to its body on the next cache line, so
  indirect transfers through the label value still work while steady
  state never executes from the line that data shares;
- direct rel32 transfers (zero-addend pc-relative patches) resolve
  to the target's body, skipping the sled so hot calls never fetch
  the shared line; the ELF entry point also targets the body;
- a code item that precedes data ends with trailing guard lines
  (two; the instruction prefetcher runs a line or two ahead) inside
  its own translation.

Measured on the binary-calculator workload (1M operations, 16 MB
input): 8.8s before, 0.30s after, output identical.

The entry point is the label `start` if present, otherwise the first
statement's address.

## Validation plan

- `make test-pa9` then root `make test-report-through-pa9` (exit
  criterion); scoped `make test-report ACTIVE_TEST_REPORT_PAS='pa9'`
  for iteration.
- Cross-check single instructions against a production assembler
  (`gcc -nostdlib` + `objdump -d -M intel`) when an encoding is in
  doubt.
- `perl scripts/cppgm_file_audit.pl --stage pa9 --paths dev/src`
  for size/structure gates.

## Architecture Review

The implementation matches the pipeline above; the audit (see
`audit.md`) confirmed each stage's ownership against the code:

- The driver (`dev/cy86.cpp`) runs the unmodified PA5 preprocessor
  and phase-7 tokenizer per srcfile and concatenates the token
  sequences; CY86 knowledge starts at the parser. There are no
  test-shape or test-name gates and no fallback success paths: every
  diagnosis throws, and `main` maps any throw to EXIT_FAILURE.
- Semantic facts are typed, not stringly. The opcode table is built
  once from the `cy86-opcode.desc` transcription into
  `CY86OperandSpec` (written/imm-only flags + bit width) and an enum
  family/variant that carries signedness, floatness, relation, and
  syscall arity into codegen. Operand width conversion happens once,
  in the parser, where the constraint is known; codegen consumes
  converted bytes or symbolic label+addend immediates and never
  re-derives values from token bytes.
- Labels are interned to dense ids in the parser; codegen forwards
  them in patches; the image resolves them once at layout. No
  component recovers label facts downstream of their owner.
- The encoder is form-driven (ModRM/REX/prefix machinery shared by
  all mnemonics) rather than per-opcode byte tables, which is the
  extension point later backend assignments need.
- Every label-bearing encoding has a value-independent length
  (mov r64, imm64, absolute [disp32], rel32 transfers, and
  fixed-width data items), so layout is a single sequential pass
  plus one patch pass; there is no fixed-point relayout loop to go
  quadratic.

One semantic bug was found by differential probing against
`cy86-ref` and fixed: the +-literal offset term of label immediates
and memory displacements negated the literal at its own width before
extension, so `(label - 1u)` added 0xFFFFFFFF instead of -1, and
`[reg - "ab"]` (where the minus is subtraction of the converted
64-bit value, not arithmetic literal negation) was wrongly rejected.
`OffsetLiteral` now converts to 64 bits first and then negates
modulo 2^64, matching the reference.

## Final Architecture Review

Re-reviewed after the audit fixes, against the exit criteria:

- `make test-report-through-pa9`: 384/384 tests across pa1-pa9, all
  stages passing; the PA9 fixtures execute the generated ELF natively
  and diff stdout and exit status against committed reference
  fixtures, so the binary is exercised for real, not pattern-matched.
- Differential probes beyond the fixtures (alignment, width
  conversion, 8/16-bit mul/div/mod, shifts, partial-register writes,
  signed/unsigned/float compares, u64<->f80 boundaries, control
  transfer, data-label patch truncation, ill-formed diagnosis parity)
  agree with `cy86-ref` except for two documented stand-in gaps where
  the handout governs: the stand-in cannot compile `move80` at all
  ("not implemented yet") and does not enforce the handout's
  requirement that label +- offsets be integral literals. This
  implementation keeps `move80` fully implemented and keeps the
  required diagnosis.
- Generated code disassembles cleanly (objdump cross-check) with the
  intended fixed sequences and REX-correct forms.
- No interpreter, trampoline, template binary, or embedded payload
  anywhere: the output is straight-line translated machine code laid
  out by the image writer.
- Performance: parsing, translation, encoding, and layout are all
  linear in program size; table lookups are O(log n) over 170
  opcodes. The full PA9 suite (build + 11 program runs) completes in
  about a second.

## Later-assignment fit

The X86Instruction model and encoder are the seed of the real backend
(PA10+ `lowir2native`): mnemonics/forms are added per need, memory
operands support [base+disp] generally, and the patch/layout
machinery lives with the image, not with CY86. The CY86 parser and
translator stay small and CY86-specific by design (the handout says
CY86 is not the future IR).
