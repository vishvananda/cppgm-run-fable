# PA9 (cy86) Audit

## Audit Plan

Scope: commit 16bd6d0fd (Implement PA9 cy86) against `pa9/README.md`,
`pa9/plan.md`, `pa9.gram`, and `cy86-opcode.desc`.

Files to inspect:

- `dev/cy86.cpp` — driver: argument handling, per-TU preprocessing,
  failure paths (no fallback success, no test-shape gates).
- `dev/src/cy86/cy86_parser.{h,cpp}` — opcode table fidelity against
  `cy86-opcode.desc`; all handout diagnoses (keywords, user-defined
  literals, label collisions, arity/kind/width, non-integral label
  offsets, undefined labels); PA9 width-conversion and negation rules
  over PA2 value bytes; ownership of the label table.
- `dev/src/cy86/cy86_codegen.cpp` — per-family x86 sequences:
  operand staging, signedness from family/variant (not strings),
  x87 stack balance, u64<->f80 range fixes, syscall argument staging,
  red-zone discipline; no interpreter/trampoline/embedded payloads.
- `dev/src/x86/x86_encoding.{h,cpp}` — encoder correctness (REX,
  byte-register REX, 0x66 prefix, ModRM/SIB/disp), value-independent
  label-bearing encodings, patch bookkeeping.
- `dev/src/x86/elf_program.{h,cpp}` — layout/alignment ownership,
  label resolution, patch truncation widths, ELF header fields.
- `dev/frontend_source_sets.mk`, `dev/.gitignore` — build wiring and
  the anchored ignore entry.

Cheating-surface checks specific to PA9:

- The harness executes the generated ELF natively and diffs stdout +
  exit status against committed ref fixtures, so dummy output or an
  interpreter substitute cannot pass; verify the binary is a real
  translation (disassemble a generated test program) and that no
  test-name or source-shape gates exist in the driver or harness.
- Verify the opcode table is a faithful transcription of
  `cy86-opcode.desc` (machine-diff all 170 entries).
- Verify diagnoses fire for ill-formed programs rather than producing
  a best-effort binary (impl exit status must match ref).

Differential probes against `cy86-ref` (beyond the 11 fixtures):

- literal-statement alignment (int/double/long-double/string element);
- data8/16/32/64 alignment padding and label-valued data patches
  (truncation widths);
- label immediate +/- signed and unsigned offsets (sign- vs
  zero-extension of the offset literal);
- immediate width conversion: truncation and sign/zero extension,
  including char (signed) and char16_t/char32_t (unsigned);
- 8-bit mul/div/mod signed and unsigned (ah remainder path);
- shifts at width boundaries; 32-bit writes zeroing upper halves,
  8/16-bit writes preserving them;
- u64convf80 with the top bit set and f80convu64 at/above 2^63;
- float compares including the operand order of fcomip;
- move80 register/immediate/memory combinations;
- entry point: `start` label vs first-statement default;
- ill-formed cases: keyword present, UD literal, duplicate label,
  label colliding with register/opcode, undefined label, wrong
  register width, immediate as written operand, missing/extra
  operands, non-integral label offset.

Ownership boundaries to confirm:

- parser owns width conversion of literal immediates (codegen never
  re-derives them from token bytes);
- codegen owns CY86->x86 mapping; encoder owns byte format; image
  owns layout/patches — no downstream re-derivation of label values;
- signedness/floatness flow as enum family/variant, not as strings.

Performance risks to inspect:

- per-identifier register lookup (linear over 18 entries) and
  per-opcode map lookup — bounded, fine;
- statement translation buffers copied into ImageItem and again into
  the payload — sizes are small and linear overall;
- layout is one pass plus one patch pass — no quadratic scans;
- confirm suite wall time has no outlier tests.

File-audit issues: run
`perl scripts/cppgm_file_audit.pl --stage pa9 --paths dev/src`;
confirm no size-gate bypasses (no hidden fragments, no code moved to
unchecked paths; `dev/cy86.cpp` is the tool entry and is expected to
be outside `dev/src`, matching every earlier tool).

Regression check: `make test-report-through-pa9` (all stages).

## Findings

1. **Bug: +-offset literals negated before extension.** The handout's
   `label OP_MINUS TT_LITERAL` immediates and `[base - TT_LITERAL]`
   displacements were computed by negating the literal at its own
   width and then zero/sign-extending. The reference negates after
   the 64-bit extension (equivalently: subtracts the converted
   value), so the two models diverge for unsigned literals narrower
   than 64 bits: `(buf - 1u)` must be `buf - 1`, not
   `buf + 0xFFFFFFFF`. Confirmed by differential probes (`memneg`,
   `widthconv`): the reference reads `[buf2 - 8u]` as `buf2 - 8`
   while the old code computed an address 4GiB away and crashed.
   The same root cause wrongly applied the "arithmetic literals
   only" negation rule to memory displacements, rejecting
   `[reg - "ab"]`, which the reference accepts as subtraction of the
   converted 64-bit value (verified at runtime: `[y64 - "a"]`
   subtracts 97). Note the plain negated-literal immediate `(-1u)`
   *is* bytes-level negation then zero-extension (0xFFFFFFFF) in the
   reference too — only the +-offset term changes.
2. **Stand-in reference gaps (no change made, handout governs).**
   The bundled `cy86-ref` is a rebuilt stand-in, not the original
   course binary. Probing found two places it falls short of the
   handout: (a) it cannot compile any `move80` statement ("native
   float80 moves not implemented yet"); (b) it does not enforce
   "the label +-literal form requires the literal be of integral
   type", accepting `(m + 1.5)`, `[m + 1.5]`, and `(m + "ab")`.
   Crippling `move80` or dropping the required diagnosis to mimic a
   stub would be a deliberate regression; both are kept per the
   handout. No committed fixture exercises either case.
3. **Layout base differs from the reference** (payload directly
   after the headers at 0x400078 versus the reference's higher
   base). Absolute addresses are not specified by the assignment and
   are unobservable in the sanctioned stdout/exit-status protocol;
   all *relative* layout behavior (literal-statement alignment
   including the 16-byte long double, dataN self-alignment, string
   element alignment) matches the reference exactly. Acceptable.
4. **Opcode table is a faithful transcription**: machine diff of all
   170 entries of `cy86-opcode.desc` against `kRawOpcodes` found no
   missing, extra, or mismatched specs.
5. **Keyword detection is enum-ordering based but sound**: every
   `KW_*` precedes `OP_LBRACE` in `ETokenType`, and user-defined
   literal kinds fall to the explicit invalid-token diagnosis.
6. **Minor: `CY86Opcode::spec` was a write-only string field**
   duplicating the parsed operand constraints. Removed.
7. **No cheating surfaces found**: the harness executes the
   generated ELF natively and byte-compares stdout plus exit status
   against committed fixtures; the driver contains no test-name or
   source-shape branches, no embedded payloads, and no fallback
   success path (every diagnosis throws; main maps throws to
   EXIT_FAILURE). Generated code disassembles to the intended fixed
   sequences (objdump cross-check of a probe binary).
8. **Performance is linear**: one token pre-pass, one parse pass,
   fixed-size translation per statement, one layout pass plus one
   patch pass. Register lookup is a bounded 18-entry scan; opcode
   lookup is O(log 170). Full suite (build + 11 native program runs)
   completes in about a second; `make test-report-through-pa9` in
   ~15s.

## Changes Made

- `dev/src/cy86/cy86_parser.cpp`: `OffsetLiteral` now converts the
  literal to 64 bits first (sign-extend signed integral, else
  zero-extend) and then negates modulo 2^64 when `OP_MINUS` is
  present, instead of negating the literal bytes at their own width
  before extension. This fixes unsigned (and non-arithmetic)
  +-offset terms in label immediates and memory displacements and
  stops rejecting non-arithmetic literals in displacement
  subtraction, where the minus is address arithmetic rather than
  literal negation. The integral-type requirement for *label*
  offsets is retained per the handout.
- `dev/src/cy86/cy86_parser.{h,cpp}`: dropped the write-only
  `CY86Opcode::spec` field (the parsed `operands` vector is the
  single owner of the constraint data; `kRawOpcodes` retains the
  transcription strings).
- `pa9/plan.md`: added Architecture Review and Final Architecture
  Review sections.

## Validation

- `make test-report-through-pa9`: 384/384 tests, pa1-pa9 all
  passing (run after the fixes).
- `perl scripts/cppgm_file_audit.pl --stage pa9 --paths dev/src`:
  pass (one pre-existing warning about `dev/src/parse/parser.h`, a
  PA6 file untouched by PA9).
- Differential probes vs `cy86-ref` (in addition to the 11
  fixtures), all matching after the fix: literal/data alignment
  (relative), width conversion and negation (`widthconv`), 8/16-bit
  mul/div/mod + shifts + partial-register writes (`arith8`),
  signed/unsigned/float comparisons (`cmp`, `floats`), u64<->f80
  boundary conversions and x87 arithmetic (`floats`), control
  transfer including call/ret/jumpif and memory-indirect jump
  (`control`), data-label patch truncation with anchored alignment
  (`datalabel`), unsigned displacement subtraction (`memneg`),
  string-literal displacement subtraction (`strdisp`).
- Ill-formed diagnosis parity battery (25 cases): agreement with the
  reference on every case except the three forms covered by the
  stand-in's missing integral-offset check (Findings #2), where the
  handout's required diagnosis is kept.
- objdump disassembly of a generated probe binary decodes cleanly to
  the intended sequences (REX-correct byte-register forms,
  value-independent movabs label slots, rel8 skip branches).
