# PA13 Plan: `lowir2cy86`

PA13 adds a backend adapter that parses the PA13 LowIR subset, validates it
structurally, and translates it into deterministic PA9 CY86 source text. The
oracle is exact text comparison against `tests/spec/*.ref` for successful
runs and `EXIT_FAILURE` for malformed inputs.

## Ownership Boundaries

- `dev/lowir2cy86.cpp` stays a thin driver: argument handling, file reading,
  and exit-status policy. All real work lives in `dev/src/lowir/`.
- `dev/src/lowir/` owns the LowIR family front half (model, lexer, parser,
  validator) plus the PA13 CY86 emission back half. Later assignments
  (`cppgm++ --emit-lowir`, `lowiropt`, `lowir2native`) reuse the model,
  lexer, parser, and validator unchanged; only the CY86 emitter is
  PA13-specific.
- The CY86 emitter produces text only. It does not reuse `dev/src/cy86/`
  (PA9 parses/encodes CY86; PA13 only writes it), and nothing here shells
  out or consults reference binaries.

Files:

- `lowir/lowir_program.h` + `lowir_program.cpp` — typed IR model: types
  (`void`, int/float scalars, `ptr`, `obj<NxA>`), operands, metadata maps,
  instructions, blocks, functions, globals, aliases, program. Small helpers
  (type classification, sizes) live here so the validator and emitter share
  one source of truth.
- `lowir/lowir_lexer.h/.cpp` — tokenizer over the concatenated input text.
  Tracks line numbers (the only line-sensitive parse decision is the
  optional `return` value). Lexes `@g`/`%t`/`$s`/`^b` names, numeric
  literals (sign handled by the parser), `NxA` spans, `obj<NxA>` types,
  punctuation, and `!dbg(file, line, col)` suffixes as single tokens.
- `lowir/lowir_parser.h/.cpp` — recursive descent over `pa13.gram`,
  producing the model. Any grammar violation throws; the driver maps that
  to `EXIT_FAILURE`.
- `lowir/lowir_validate.h/.cpp` — structural validation (below).
- `lowir/lowir_frame.h/.cpp` — per-function frame layout (below).
- `lowir/lowir_to_cy86.h` (internal header) + `lowir_emit_program.cpp`,
  `lowir_emit_inst.cpp`, `lowir_emit_values.cpp` — CY86 text emission,
  split as program/global structure, per-instruction translation, and
  operand-load/store + f80 helpers, each under the audit size limits.

## Validation Plan

Reject (exit `EXIT_FAILURE`) on:

- any lex/parse error against `pa13.gram`
- duplicate top-level symbol names across all declarations and definitions
- duplicate `alias object` spellings; alias targets that are not top-level
  declarations or definitions
- `tls_for` targets that are not thread-local globals; more than one
  wrapper per thread-local global
- duplicate parameter, slot, or block names within a function; functions
  with no blocks; blocks without exactly one trailing terminator
  (terminators: `jump`, `branch`, `switch`, `return`, `throw`, `resume`);
  instructions after a terminator; control transfers to unknown blocks
  (`jump`/`branch`/`switch`/`eh_try`/`eh_cleanup`)
- temporaries used before definition (params count as defined); unknown
  slots/globals/functions where the contract requires definitions
- unknown metadata keys, unknown metadata values, metadata on the wrong
  kind (function roles vs global roles, `storage` on functions, top-level
  symbol metadata on call signatures), duplicate singleton roles
- parameter metadata on non-`ptr` parameters (`pass` other than `direct`,
  `capture`, `access`, `alias`); `indirect_result` not first or on a
  non-`void` function
- indirect calls (temporary or data-global callees) without `as (...)`
  signatures; direct-call argument counts that contradict the declared
  arity (`fixed` exact; `variadic`/`prototype_relaxed` at least the prefix)
- `convert` width violations (`zext`/`sext` must widen, `trunc` must
  narrow, int/float operand kinds must match the operator), `unary decay`
  on non-`ptr`
- `copyobj`/`zeroinit` spans whose alignment is not a power of two
- entry resolution failures: no `[role=entry]` definition and no legacy
  `@main` definition, or more than one

Diagnostics are `std::runtime_error` text; only the exit status is graded.

## CY86 Emission Contract

All facts below were derived from the checked-in refs and are implemented
as fixed templates so translation stays monotonic.

Symbols: function `@n` → `fn__n`, global `@n` → `g__n`, block `^b` of `@f`
→ `fn__f__b`, plus `fn__f__epilogue`. Synthetic EH runtime:
`fn____cppgm_eh_unhandled`, `g____cppgm_eh_top`, `g____cppgm_eh_value`,
emitted only when EH instructions appear. Synthetic control labels use one
program-wide counter: `__atomic_cmpxchg_success__N` / `__atomic_cmpxchg_end__N`,
`__eh_handler__N` / `__eh_unhandled__N`.

Program layout: `start:` block (init hook call if present, entry call,
fini hook call wrapped in an x64 save/restore if present, `syscall1 t64 60
x64`), then function definitions in source order, then the EH runtime
function, then globals in source order, then the EH globals; sections
separated by blank lines.

Frame layout per function, allocated top down from `bp`:

1. one synthetic 8-byte slot holding the hidden result pointer when the
   return type is `f80` or `obj<...>`
2. parameter homes in order (8 bytes for scalars/`ptr`; 16 for `f80`;
   `obj<NxA>` rounded up to 8)
3. slot homes in order (same sizing)
4. temporary homes in first-definition order (`cmp` and `index` results
   are 8; `convert`/`call`/`const`/`load`/`copy` size by their type),
   with one anonymous 16-byte home per f80 literal call argument
   allocated in the same walk (each by-address literal needs storage
   that survives until the `call`; a shared scratch slot cannot hold two
   of them at once)
5. a 64-byte scratch tail (four 16-byte slots `S1..S4` at offsets
   `homes+16k`) whenever the function contains any `convert` instruction
   or mentions `f80` anywhere (params, return, slots, instruction types)

`frame = homes (+64 if scratch)`; `isub64 sp sp frame` is omitted when 0.
Parameters arrive in `x64,y64,z64,t64` (the hidden result pointer is
argument 0), then on the caller stack at `[bp+16+8k]`. `f80`/`obj` values
pass by address; callees copy them into their homes 8 bytes at a time.

Value loads are width-typed (`move64` for 64-bit types, `move32` for
32-bit, zero-`move64` + `move16`/`move8` for narrower); literals keep
their source spelling and always load with full-width moves (`move32` for
`f32`); slot operands in value position yield their address. `f80` values
move through scratch slots as two 8-byte words with explicit zero padding
(`move32`/`move16`) after every 10-byte write; float conversions all
route through `f80` scratch using the `*convf80`/`f80conv*` opcodes.

`cmp` results are canonical `i64` 0/1 (`<pred> z8`, then `move64 x64 0;
move8 x8 z8`). Binary ops load lhs→`y`, rhs→`x`, result in `x`. Shifts
move the count through `z64`/`x8`. Atomics use the single-threaded
expansions from the README (cmpxchg branches through synthetic labels).
`copyobj`/`zeroinit` expand to chunked moves through `z64` with pointer
increments between chunks. The EH templates push/pop 32-byte handler
records on the CY86 stack keyed off `g____cppgm_eh_top`.

Global data: scalar globals emit one `data<w>` line preserving literal
spelling (`zero` → `0`, `addr @s` → label, addends as `(label+k)`);
structured globals emit items in order with explicit `data8 0;` padding to
each item's natural alignment; `zero N` emits N `data8 0;` lines. `f80`
data encodes the host 80-bit pattern as `data64` (signed) + `data16` +
six `data8 0;` lines.

## Validation/Test Plan

- `make -C pa13 test` for the local suite; per-case diffs of `x.my`
  against `x.ref` while iterating.
- `make test-report-through-pa13` as the exit criterion, preserving
  PA1-PA12.
- `perl scripts/cppgm_file_audit.pl --stage pa13 --paths dev/src` clean.

## Architecture Review

The implementation matches the plan's ownership story. The driver
(`dev/lowir2cy86.cpp`, 96 lines) only handles argv, file concatenation,
and exit-status policy; every LowIR fact lives in `dev/src/lowir/`. The
pipeline is strictly lex → parse → validate → translate with no phase
skipped and no fallback success path: any failure throws and the driver
maps it to `EXIT_FAILURE` without writing usable output. Nothing in the
tool consults reference binaries, test names, or input paths.

Boundaries hold as planned: `LowIRType` is the single owner of
size/width/element facts consumed by validator, frame layout, and
emitter; the validator alone owns legality tables and resolves
entry/init/fini hooks into `LowIRProgramInfo`, which the emitter consumes
without re-deriving metadata; the CY86 emitter writes text only and
shares nothing with `dev/src/cy86/`. Semantic facts are typed enums
(types, opcodes, operand kinds, literal classes); metadata stays
textual key/value pairs because preserving the textual call-boundary
contract is the point of the LowIR format — PA13 validates values
against closed tables and translates only the facts the adapter needs
(roles, storage, arity).

Performance is linear end to end: one token vector, one recursive-descent
pass, one validation walk with map/set symbol tables, one frame walk per
function, one emission walk appending to a single output string. The
EH-usage scan short-circuits on first hit. No quadratic scans, no
repeated whole-program walks, no hot-path recomputation.

## Final Architecture Review

The PA13 audit (see `pa13/audit.md`) confirmed the structure above and
fixed one latent miscompile found during review: f80 literal call
arguments were materialized into a scratch slot that (a) was not
allocated when the call was the function's only f80 mention — placing
the bytes below `sp` where the `call` clobbers them — and (b) was shared
by every f80 literal in one call. Each such literal now gets its own
16-byte frame home allocated during the existing frame walk, and
by-address literals with non-f80 spellings are rejected at translation
instead of emitting malformed `move80` text. All 90 checked-in outputs
are byte-identical before and after the fix.

One inherited template quirk is documented rather than changed: the
checked-in refs route by-address argument addresses through `x64` before
moving them into later unit registers, which clobbers a unit-0 value
already in `x64` (visible in `200-f80-direct-call.ref`). PA13 grades
exact text equality, so the adapter reproduces the oracle's sequence;
any later assignment that executes adapter output must resolve that
template conflict on its own terms.

Exit criteria: `make test-report-through-pa13` passes (784/784, 13/13
stages) and `perl scripts/cppgm_file_audit.pl --stage pa13 --paths
dev/src` passes with only the pre-existing PA10 `parse/parser.h`
warning.
