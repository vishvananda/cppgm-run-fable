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
- `x86_encoding` owns the X86Instruction object model (mnemonic +
  Reg/Mem/Imm operands) and the generic encoder (legacy prefixes,
  REX, opcode, ModRM, SIB, disp, imm). Immediates carry an optional
  symbolic label reference; every label-bearing form encodes with a
  value-independent length (mov r64, imm64 / fixed-width data), so
  statement sizes are final on the first pass.
- `elf_program` owns layout and output: items are code bytes or data
  blobs with alignment (literal statements pad to their PA2 type
  alignment, dataN to N/8); offsets are assigned sequentially, labels
  bind to their statement's post-padding address at 0x400000 + 120 +
  offset, recorded patches (label value + addend, truncated to the
  patch width) are applied, and the ELF header + one RWX PT_LOAD
  segment + payload is written and chmod 0755.

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

## Later-assignment fit

The X86Instruction model and encoder are the seed of the real backend
(PA10+ `lowir2native`): mnemonics/forms are added per need, memory
operands support [base+disp] generally, and the patch/layout
machinery lives with the image, not with CY86. The CY86 parser and
translator stay small and CY86-specific by design (the handout says
CY86 is not the future IR).
