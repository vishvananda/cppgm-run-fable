# PA29 Plan: `cppgm++ -c` and Link Mode

PA29 turns the PA10-PA28 pipeline into a practical compile-and-link
toolchain: `-c` compiles one C++ translation unit into a compiler object
file, and default link mode combines compiler objects, C++ sources, and
host-built helper objects (`-L`/`-l`) into a native Linux executable.
This is the first stage that *executes* compiled C++ programs natively,
so it also brings up the missing execution-side machinery: exception
handling in the x86 backend and a freestanding C++ runtime library.

## Pipeline

Per translation unit (`-c`, and each source input in link mode):

1. Phases 1-7 + parse + sema (the PA10-PA12 front end, unchanged).
2. `LowerProgram` with exactly one unit, in a new separate-compilation
   mode (whole-program-only folds stay off so `-c` output is valid
   under any link).
3. The canonical LowIR text (the same `--emit-lowir` emission), then
   `ParseLowIRProgram` + `ValidateLowIRProgram(require_entry=false)`.
   The LowIR text remains the frontend/backend contract; the parser and
   validator are extended to accept everything the emitter writes
   (`eh_catch @rtti, N`, `exception_selector`, `object_root=yes`,
   `object=@self` merge keys).
4. `LowerLowIRProgramToMir` (PA28 backend) without the startup sled.
5. Native encoding into relocatable form: image items + symbolic
   patches + a symbol table (see Object Format), serialized to the
   object file.

Link mode compiles each source input through the same five steps into
an in-memory module, reads compiler objects and host ELF objects into
the same module shape, and links. Direct-vs-separate parity is by
construction: both paths run identical per-TU compilations.

## Object Format (internal contract, replaced by PA31/PA32)

A `CPPGMOBJ` binary file holding one module:

- target name (normalized; `x86_64-unknown-linux-gnu` == `linux`),
- items: alignment, code/data flag, bytes (encoded x86 or data image),
- patches per item: offset, size, kind (abs64/abs-disp32/pcrel/trunc),
  module-local symbol index, addend,
- symbols: low name, external name, binding
  (undef/strong/weak/internal), defining item + offset (or undefined),
- role facts: entry/init/fini symbol indices, TLS wrapper map.

Symbol naming: the external name is the LowIR `object=` value when
present (bare Itanium spelling; `object=@low` self-merge keys use the
low name, which is deterministic across TUs by construction), otherwise
the low name itself (`main`, backend-generated runtime references).
`binding=internal` symbols never participate in cross-module
resolution. `alias object X = @y` lines become additional symbols on
`y`'s item. Encoding assigns labels to referenced-but-undefined
symbols; unreferenced declares impose nothing on the link.

## Linker (`dev/src/toolchain/`)

- Global table over strong/weak definitions keyed by external name.
  Two strong definitions of one name: error. Strong beats weak; first
  weak wins otherwise (COMDAT-style; losing items become dead bytes).
- Undefined references resolve through that table; symbol offsets fold
  into patch addends. Unresolved references after adding the built-in
  runtime module: error. No `role=entry` definition: missing-main
  error. All modules must agree on the target.
- Startup synthesis: the linker builds a tiny MIR module - item 0
  calls every module's `role=init` hook in link order, calls the entry,
  saves the result across the `role=fini` calls (reverse order), and
  exits. It also defines `__cppgm_unwind_raise` (pop the innermost
  handler record, reset the selector, restore rbp/rsp, indirect-jump
  to the dispatch address; exit(134) when no handler) and
  `__cppgm_exit`.
- Layout and writing reuse `ProgramImage` unchanged. PA29 code items
  are aligned to 64 bytes so no entry sleds are inserted and
  symbol+addend patches (block addresses, ELF section offsets) resolve
  exactly.
- The runtime library is compiled on demand: if unresolved names
  remain after normal inputs, the linker compiles the built-in runtime
  TU (once) through the ordinary pipeline and retries. No name lists,
  no test-shape gating.

## Host ELF objects (`-L`/`-l`, `elf_reader`)

`-l name` resolves `lib<name>.o`/`.obj` across `-L` dirs. ET_REL ELF64
readers map allocatable sections to items (64-byte aligned code;
`.bss` becomes zero items; `.eh_frame`/notes dropped), symbols to
module symbols (section offsets kept), and `.rela.*` entries to
patches: PC32/PLT32 -> pcrel with `addend + 4 + symbol offset`, 64 ->
abs64, 32/32S -> 32-bit absolute. Our generated calls already follow
the SysV ABI (args rdi/rsi/rdx/rcx/r8/r9 + xmm, 16-byte call-site
alignment), so `extern "C"` interop needs no shims.

## Exceptions and RTTI at native level

LowIR EH ops lower to a handler-record chain (the design the CY86
backend already uses, adapted to real registers):

- `eh_try`/`eh_cleanup`: build a 32-byte frame-resident record
  {prev, dispatch address, rbp, rsp}, push onto the chain head global
  `__cppgm_eh_top`. Dispatch addresses are function-label+block-offset
  ABS patches back-filled when the function's block offsets are final.
- `eh_end`: pop the chain head.
- `throw`/`resume`: enter `__cppgm_unwind_raise`.
- `eh_catch @rtti, N` / `eh_catch_all, N`: call
  `__cppgm_eh_match(rtti-or-null, N)`; first match sets the selector
  and the adjusted pointer. `exception` / `exception_selector` load
  the runtime's current-exception globals.

Register discipline: a function containing EH ops compiles in a
conservative mode - every value gets a frame home and cached register
locations invalidate at block boundaries - because unwinding restores
only rbp/rsp, so dispatch blocks are entered with dead registers.
Functions without EH ops keep the exact PA28 register discipline, so
the PA28 strict fixtures are unaffected.

The runtime library is a freestanding C++ TU (compiled by this same
compiler at link time) providing: `__cxa_allocate_exception` /
`__cxa_throw` / `__cxa_begin_catch` / `__cxa_end_catch` /
`__cxa_rethrow` over a static exception arena with caught-exception
stacking, `__cppgm_eh_match` (Itanium RTTI walk: exact, class
derived-to-base through si/vmi records with public-path offsets,
pointer qualification, catch-all), `__dynamic_cast` (offset-to-top +
vtable RTTI slot, full down/cross-cast search), `__cxa_bad_cast` /
`__cxa_bad_typeid` / `__cxa_pure_virtual`, `cppgm_builtin_operator_new`
/ `_delete` over a static arena, and the `__cxxabiv1` type-info vtable
anchor objects that RTTI records point at (classification is by anchor
address; records are deduplicated by external name, so identity
compares work).

## Ownership boundaries

- `dev/cppgm++.cpp`: option parsing and mode orchestration only.
- `dev/src/toolchain/`: compile-unit pipeline, object format, linker,
  ELF reader, runtime library text. Owns all symbol-resolution policy.
- `dev/src/x86/`: instruction encoding, per-module encoding, EH op
  code generation. Knows labels and patches, never external names.
- `dev/src/lowir/`: parse/validate the full emitted LowIR language.
- `dev/src/lowering/`: unchanged except the separate-compilation flag.
- PA30+ handoff: mangling stays in `lowering/lower_name*`; PA31/32
  replace the object encoding behind the same module boundary.

## Validation

- Round-trip check: every pa29 test source emits, parses, and
  validates as LowIR before backend work lands.
- `make test-pa29` for the driver suite (each test checks explicit
  compile+link, direct source link, and mixed parity plus program
  output), then root `make test-report-through-pa29` - the PA28
  fixtures pin the register allocator, and PA13-27 pin the front end.
- Negative driver tests (duplicate strong symbol, unresolved external,
  missing main) exercise the linker error paths.
- `perl scripts/cppgm_file_audit.pl --stage pa29 --paths dev/src`.
