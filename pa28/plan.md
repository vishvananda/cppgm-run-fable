# PA28 Plan: lowir2native

## Contract

`lowir2native` parses PA13-surface LowIR text, lowers it to a typed
x86-64 machine IR (MIR), dumps that MIR deterministically with
`--dump-machine-ir`, and encodes the same MIR into a static Linux ELF
executable with `-o`. Tests compare the compiler exit status, the raw
MIR (strict suite), the canonicalized MIR (structural suite), and the
generated program's stdout/exit status. The `.ref` fixtures are the
oracle; the strict suite pins exact register choices, so the lowering
discipline below is normative.

## Pipeline and ownership

1. **Parse + validate** — reuse the PA13 LowIR front half unchanged:
   `dev/src/lowir/lowir_lexer|parser|validate`. The typed
   `LowIRProgram` model already covers the full current surface
   (metadata, declares, structured globals, atomics, obj types, EH
   markers, aliases). One extension: `ValidateLowIRProgram` gains a
   `require_entry` option so helper-only inputs (no `[role=entry]`)
   still lower for PA28; the dumped MIR then omits the `startup`
   section. PA13's driver keeps the strict behavior.
2. **Lower LowIR -> MIR** — new `dev/src/x86/lowir_to_mir*` producing a
   `mir_model::MirProgram` (the provided scaffold model in
   `dev/src/mir_model.h`, extended as needed). All backend decisions
   (register assignment, frame layout, call ABI, staging) are made
   here; the dump and the encoder both consume the finished MIR so they
   can never disagree.
3. **Serialize MIR** — new `dev/src/x86/mir_serialize.cpp` implements
   `mir_model::serialize_mir_program` exactly in the fixture format.
4. **Encode MIR -> native** — new `dev/src/x86/mir_to_native*` walks the
   MIR and emits machine code through the PA9 encoder
   (`dev/src/x86/x86_encoding`) into the PA9 image writer
   (`dev/src/x86/elf_program`). The encoder grows the missing forms
   (SSE scalar ops, cvt*, lock xadd/cmpxchg, xchg, mfence, lea, neg,
   bswap two-op cmp/test with memory, setcc widths, x87 extras).
   No CY86 anywhere in the path.

`dev/lowir2native.cpp` keeps only the CLI surface. New sources register
in `FRONTEND_OBJ_BASENAMES_lowir2native`.

## MIR text format (from fixtures)

```
machine_ir x86_64 <target>
<blank>
startup                      # omitted when no entry function
    call @__cppgm_init       # only when init hook exists
    call @main
    mov r12, rax             # only when fini hook exists
    call @__cppgm_fini
    mov rdi, r12             # else: mov rdi, rax
    exit
<blank>
global @name [readonly|thread_local]
  storage scalar <type> | storage data
  init <type> <lit> | init zero | init ptr addr @sym [+ N]
  item <type> <lit> | item ptr addr @sym [+ N] | item zero <N>
<blank>
function @name
  abi
    param %p -> rdi|rsi|rdx|rcx|r8|r9|xmmN|[rbp+N] : <type>
    return <type> -> rax|xmm0|st0|void
  frame
    stack_size <N>
    scratch_bytes <0|48>
    preserve rbx r12 ...     # only when non-empty
    slot $s -> [rbp-N] : <type>
    param-slot %p -> [rbp-N] : <type>     # f80 params copied local
    temp %t -> [rbp-N] : <type>           # frame-allocated temps
<blank>
  block ^label
    <one instruction per line, 4-space indent>
```

Globals and functions appear in source order; blocks in source order.

## Register discipline (normative, reverse-engineered from strict refs)

Fixed-role registers (never canonicalized, never in the pool):
- `rax` — value staging (immediates for store/return, division,
  atomic results, setcc chains), return register.
- `rcx` — address staging (load/store base via temp addr), division
  divisor / shift count, atomic address staging, switch case staging.
- `rdx` — literal RHS staging for cmp/binary, division high half,
  atomic cmpxchg expected-pointer staging.
- `rsi`, `rdi` — argument registers, bulk-op (copy/zero_bytes)
  operands, atomic cmpxchg desired value (rsi).
- `r10` — indirect call target staging. `r11` — TLS address result,
  obj-return address staging.
- `xmm0`, `xmm1` — float staging/ABI; `xmm2..7` are the float pool.

Allocation pool, scanned in order:
- plain temps: `r8, r9, rbx, r12, r13, r14, r15`
- param values: same pool minus `r8` in practice (see param
  pre-assignment below — r8 is consumed by a forwarded param's
  reservation, producing the observed r9-first behavior)
- values live across a call (or an instruction containing a call,
  e.g. TLS access): callee-saved subset `rbx, r12, r13, r14, r15`

Rules:
- Values get a register at definition while operands are still live;
  freed after their last use.
- In-place reuse: when the LHS operand is a dying non-param temp, the
  destination reuses its register (`add r9, rbx`, `lea r8, [r8+8]`,
  `sete rbx` chains). Params never die (pinned to end of function), so
  results derived from params always copy first (`mov r12, rbx; lea
  r12, [r12+8]`).
- Params are pre-assigned pool slots in declaration order at entry:
  multi-use params take the callee-saved pool; single-use params take
  the scratch-first pool and reserve their slot even when the copy is
  later forwarded away (this is what leaves r8 unused in functions
  whose first param is forwarded). Forwarding (reading the incoming
  arg register directly, no copy) applies to single-use params whose
  use is mov-like: store-value, return-value, copy source, binary-LHS
  dest-copy. Memory bases/addresses and binary-RHS uses force a copy.
- Pool exhaustion at a definition point spills the value to an
  anonymous 8-byte frame slot (call results get named `temp %t` frame
  entries; pre-call conflict spills are anonymous).
- Values whose address is required (by-address call args) are
  allocated `temp` frame slots at definition and rematerialized with
  `lea`.
- `addr $slot` temps are never allocated; each use rematerializes
  (`lea rcx, [rbp-N]` at load/store bases, `lea rdi/rsi, ...` at call
  args and bulk ops, `lea dest, ...` for index bases).

## Instruction templates (strict-suite shapes)

- const/copy: `mov dest, imm` (+ `sext.i32/zext.*` narrow
  normalization); copy of temp: `mov dest, src`.
- load: `load.<ty> dest, @g | [rbp-N] | [reg]`; narrow loads append
  `sext.i8/16/32` or `zext.u8/16/32`-style normalization
  (`sext.i32 r8`, `movzx`-backed forms per fixtures). Load feeding an
  immediately-following single-use `return` or store-value collapses
  into `rax`.
- store: `store.<ty> base, valreg`; literal values stage through
  `mov rax, imm` first.
- binary: `mov dest, lhs; op dest, rhs`; literal RHS stages in `rdx`.
  Division: `mov rcx, rhs; mov rax, dest; cqo|mov rdx,0;
  idiv|div rcx; mov dest, rax|rdx`. Shifts: `mov rcx, rhs;
  shl|shr|sar dest, cl`. Narrow results re-normalize.
- cmp as value: `mov dest, lhs(or in-place); cmp.<ty> dest, rhs;
  set<cc> dest; movzx dest, dest`.
- compare-fed branch (single use feeding `branch`): fuse to
  `cmp.<ty> a, b; j<cc> ^then; jmp ^else` per the direct-branch goals;
  branch on plain bool: `mov rax, src; cmp.i64 rax, 0; jne ^then;
  jmp ^else`.
- switch: stage input in `rax`, each case `mov rcx, case; cmp.i64 rax,
  rcx; je ^case`, then `jmp ^default`.
- calls: args staged in ABI order (`rdi, rsi, rdx, rcx, r8, r9` /
  `xmm0..7` / stack pushes via `sub rsp` region); sources living in
  r8/r9 spill to anonymous frame slots first; indirect targets stage
  in `r10` (`call *r10`); f80 args copy onto a `sub rsp, N` region.
  Results: rax (int/ptr/obj<=8, stored via `r11` staging when the
  consumer is memory), xmm0 (f32/f64), `fstp.f80 [temp]` (f80).
- copyobj/zeroinit: `mov rdi, dst; mov rsi, src; copy_bytes BxA, rdi,
  rsi` / `zero_bytes BxA, rdi` (lea directly into rdi/rsi for slot
  addresses).
- atomics: relaxed/acquire loads and release stores are plain
  load/store; seq_cst store is `xchg`; exchange is `mov rax, val;
  xchg.<ty> [ptr], rax`; add_fetch is `mov rcx, ptr; mov rdx, delta;
  mov rax, delta; lock_xadd.<ty> [rcx], rax; add rax, rdx`;
  compare_exchange is the rcx/rdx/rax/rsi template with
  `lock_cmpxchg`, store-back through the expected pointer, then
  `sete`; thread fence seq_cst is `mfence`; signal fence emits
  nothing.
- TLS globals: every access goes `tls_addr r11, @wrapper` then
  `load/store [r11]`; stored values stage into a callee-saved register
  first (the wrapper call clobbers scratch).
- f80: entirely frame-resident. 16-byte `temp`/`param-slot` frame
  entries, `fmov.f80 [dst], [src]|lit`, three-address
  `fadd/fsub/fmul/fdiv.f80 [dst], [a], [b]`, `feq/...f80 gpr, [a],
  [b]`, `fret.f80 [slot]`, post-call `fstp.f80 [slot]`; f80 params
  pass on the caller stack (`[rbp+16]`, 16 bytes each) and copy into
  local `param-slot`s at entry. `scratch_bytes 48` is reserved
  whenever the function touches floating point or conversions.
- f32/f64: XMM-resident (`xmm0` first), `uitofp.i32.f64 xmm0, r8` /
  `fptoui.f64.i32 r8, xmm0` style conversion mnemonics that keep both
  widths visible.

## Frame layout

`stack_size = align16(8 * preserve_count + named slot/temp/param-slot
bytes + anonymous spill bytes) + scratch_bytes`, where scratch_bytes is
48 when floating point is involved, else 0. Named slots allocate top
down from rbp in declaration order (size rounded per type: scalars 8,
f80 16, obj<NxA> to its padded container). `preserve` lists the
callee-saved registers the final body actually uses, in pool order.
Some promoted param/slot cases retain residual anonymous space (helper
functions in the fixtures show align16(param-residue) beyond the rule
above); the exact retention rule is pinned by iterating the strict
suite and will be documented in code where implemented.

## Native encoding

- Image: PA9 `ProgramImage` — one RWX PT_LOAD at 0x400000, label
  patching, entry at the startup sled.
- Startup: lowered exactly from the MIR startup section; `exit` is
  `mov rax, 60; syscall` with the code already in `rdi`. When no entry
  exists, the sled is `mov rdi, 0; exit`.
- `copy_bytes`/`zero_bytes` encode as inline byte loops (or rep movsb
  /stosb) clobbering only staging registers.
- `tls_addr` calls the wrapper function and moves the result to r11;
  the wrapper is ordinary generated code. Thread-local globals in this
  single-threaded image are ordinary data; the wrapper returns their
  address.
- x87 conversion/truncation uses the PA9 fldcw truncation dance;
  f80 compares use fcomip; unsigned 64-bit conversions use the PA9
  branch-and-adjust sequences.
- Function prologue: `push rbp; mov rbp, rsp; sub rsp, stack_size`,
  then `mov [rbp-K], rbx...` for preserves; epilogue restores and
  `leave; ret`.

## Validation plan

1. `make -C pa28 test` locally per test group; use
   `make check TEST=tests/strict/100-*.t` while bootstrapping.
2. `make test-report ACTIVE_TEST_REPORT_PAS='pa28'` for the scoped
   report.
3. Strict suite first (pins the discipline), then structural, then
   behavior (register-pressure/call-clobber correctness).
4. `perl scripts/cppgm_file_audit.pl --stage pa28 --paths dev/src`
   after each layout change.
5. Full `make test-report-through-pa28` before commit; PA13-PA27 must
   stay green (shared parser/validator changes are additive only).
