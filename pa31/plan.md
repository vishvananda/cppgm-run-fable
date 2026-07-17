# PA31 Plan: Host EH Facts for `cppgm++ -c`

## Contract

`cppgm++ -c` must emit a real x86-64 ET_REL ELF object that participates in
the host C++ unwinder: Itanium landing pads, `.eh_frame` CFI, a
`.gcc_except_table` LSDA, host `__cxa_*` / `_Unwind_Resume` /
`__gxx_personality_v0` references, host-format typeinfo facts, and no private
`cppgm_eh_*` symbols. The harness links the objects with the host C++ driver
(`g++ -no-pie ...`), inspects them with `nm`/`readelf`, decodes the LSDA, and
runs the program. Test `200-host-eh-cleanup-only-resume-facts` throws from a
host-compiled object through our frame's cleanup landing pad, so the CFI and
LSDA must be genuinely correct — not just shaped right.

## Constraints discovered

- pa25/pa26/pa27 refs pin the LowIR text, including `eh_try`/`eh_catch`
  markers, RTTI globals, and metadata. Lowering output must not change.
  All PA31 work therefore lives below LowIR.
- pa25 and pa29 exercise rich EH (catch-all, function-try-blocks, catch-miss
  routing) through the private link path (`cppgm++ -c` objects re-read by
  `cppgm++` link mode and by direct source links). One codegen must serve
  both paths; the private runtime must unwind the same landing-pad code.
- pa28 MIR fixtures contain no EH opcodes, so EH codegen may change freely.
- Ref facts pin the encoding surface exactly (see "LSDA/CFI shape" below);
  extra or missing normalized facts fail the diff.

## Architecture

One EH codegen, table-driven both ways:

- LowIR (unchanged) already models the Itanium shape: `eh_try ^dispatch` /
  `eh_cleanup ^blk` arm regions; dispatch blocks carry `eh_catch @rtti, N`
  markers and route misses to the enclosing dispatch in code;
  `exception`/`exception_selector` read the landing state; `resume`
  re-raises. `__cxa_*` helpers are called via role-tagged
  `@__external_runtime__*` declares (PA30 naming: external name = host name).
- The x86 backend stops emitting the private handler-record chain
  (`MI_EH_PUSH`/`MI_EH_POP`/`__cppgm_eh_match`) and instead:
  - computes a static region map (forward dataflow of the region stack over
    blocks; landing-target in-state = arming state minus the armed region),
  - annotates every call with its innermost region,
  - synthesizes a resume-only cleanup region over each throw-payload
    construction window (between a `role=eh_allocate_exception` call and the
    following `role=eh_throw` call) when that window contains calls — this is
    what produces the reference's `call_site_has_cleanup` in
    `100-host-eh-compact-unwind-callee-save`, whose only real region is a
    callless catch-body cleanup (validated against g++ output and ref facts),
  - emits landing-pad entry code at each region-target block:
    `lea rsp,[rbp - stack_size]` (no-op under the host unwinder, restores the
    steady-state rsp under the private walker), then spills `rax`/`edx` to
    per-frame exception/selector slots (`host_eh_*` fields in `MirFunction`),
  - lowers `exception`/`exception_selector` to slot loads (dispatch entry
    blocks are also reached by intra-frame miss routing, so registers cannot
    be read directly),
  - lowers `resume` to `_Unwind_Resume(exception_slot)`,
  - records typed per-function EH facts: call-site ranges, landing-pad
    offsets, action chains (region-stack concatenation innermost-first; an
    `eh_cleanup`-armed region contributes its target-block head markers plus
    a cleanup record), ttype filter -> type symbol table, and frame facts
    (prologue advance offsets, callee-save CFA offsets, stack size) for CFI.
- Object model: `ObjectModule` carries the typed EH facts
  (`EhFunctionInfo`: code item + range, call sites, actions, ttype symbols,
  CFI program bytes). The ELF writer renders host sections from typed facts;
  the ELF reader parses host sections back to typed facts; the private linker
  consumes typed facts. Host format is the boundary encoding, typed state is
  the source of truth.
- `-c` writes ET_REL ELF via a new `toolchain/elf_object.cpp`. The private
  CPGMOBJ1 on-disk format is deleted (`LoadObjectModuleFile` already sniffs
  ELF magic; `ParseElfObjectBytes` already reconstructs modules).
- `toolchain/elf_reader.cpp` grows: recover `entry` (defined `main`),
  `init`/`fini` roles from `.init_array`/`.fini_array` relocs, and typed EH
  facts from `.eh_frame`+`.gcc_except_table` FDEs whose CIE matches our
  emission fingerprint (zPLR with direct pcrel-sdata4 personality, 0x1b);
  foreign FDEs (host helpers use 0x9b indirect) are skipped as before.
- Private link path (`link_executable.cpp` + `runtime_library.cpp`):
  - the support module synthesizes a flat region/action table from all
    modules' typed EH facts (`__cppgm_eh_regions`/`__cppgm_eh_actions` data
    items with ABS patches), plus tiny MIR primitives `__cppgm_get_frame`
    (returns caller rbp) and `__cppgm_land(lp, rbp, hdr, selector)`
    (installs rax/edx/rbp and jumps; the pad's `lea rsp` completes the
    context),
  - the runtime's `__cxa_throw`/`__cxa_rethrow`/`_Unwind_Resume` become an
    rbp-chain walker over the flat table (all our frames are rbp-based;
    values never live in callee-saved registers across landings — existing
    `eh_mode_` discipline), reusing the existing `cppgm_eh_type_matches`
    hierarchy matcher; unhandled -> exit 134 as today,
  - `__cppgm_eh_match` and the record-chain globals disappear from the
    generated-code contract.
- Runtime-owned typeinfo: host objects must show `undef _ZTIi` (libstdc++
  owns fundamental typeinfo) while class typeinfo stays a weak `define`.
  `BuildObjectModule` demotes weak defs whose external name is
  `_ZTI`/`_ZTS` of a fundamental type or pointer/pointer-to-const-fundamental
  (the exact set libstdc++ exports) to undefined references and drops their
  items; the runtime library build (new `CompileOptions` flag) emits those
  definitions itself so the private link still resolves them. Pinned LowIR is
  unaffected (the runtime module's LowIR is internal).

## LSDA/CFI shape (pinned by ref facts + host unwinder)

- `.eh_frame`: one FDE per function (all functions, so the host unwinder can
  walk through non-EH frames). Two CIE flavors: "zR" (no personality) and
  "zPLR" for functions with landing pads. Encodings: code pcrel-sdata4
  (0x1b) with PC32 relocs against the `.text` section symbol (fact:
  `reloc_section_ref text`); personality pcrel-sdata4 direct against
  `__gxx_personality_v0` (fact: `reloc_ref __gxx_personality_v0`); LSDA
  pcrel-sdata4 against the `.gcc_except_table` section symbol (fact:
  `reloc_section_ref host_lsda`). CFI: def_cfa rsp+8 / ra@cfa-8 (CIE);
  push rbp -> def_cfa_offset 16, rbp@cfa-16; mov rbp,rsp ->
  def_cfa_register rbp; after prologue: offset rules for each callee-save
  slot (slots are 8-aligned rbp-relative, recorded by the encoder).
- `.gcc_except_table` per landing-pad function: LPStart omit (0xff), ttype
  encoding 0x9b (indirect pcrel sdata4) when catches exist else omit (0xff),
  call-site encoding uleb (0x01). Call sites cover [0, fn_size) in order:
  gaps get lp=0 (facts: `call_site_starts_at_zero`,
  `call_site_has_no_landingpad`). Actions: sleb filter + sleb next-link;
  pure-cleanup call sites use action 0 (fact: `call_site_has_cleanup`).
  Ttype entries are 4-byte pcrel refs to local 8-byte slots (`DW.ref.*`
  style, STB_LOCAL in a data section) holding abs64 relocs to `_ZTI*`
  (facts: `reloc_ref _ZTIi` etc. come from the slot relocs; the 0x9b
  encoding is required by the fact decoder).
- Symbols: only defined or referenced symbols are emitted (an unreferenced
  personality declare must not create `undef __gxx_personality_v0` in
  throw-only objects); undefs dedupe by external name (user declarations of
  `__cxa_*` reuse the frontend declares — test
  `100-host-eh-runtime-declaration-reuse`).
- Calls relocate as R_X86_64_PLT32 (fact `reloc_call`), address
  materialization stays movabs/R_X86_64_64, data-word patches R_X86_64_64.
  Emit `.note.GNU-stack` to keep host linkers quiet.

## Ownership boundaries

- `dev/src/x86/lowir_to_mir_flow.cpp` (+analyze): region dataflow, call
  annotations, landing-pad synthesis, throw-window regions.
- `dev/src/x86/mir_to_native.cpp` (+`mir_native_data`): offset recording
  (prologue advances, call sites, landing pads), typed frame/EH facts out.
- `dev/src/x86/eh_frame.cpp` (new): CFI program bytes from frame facts
  (x86-owned DWARF knowledge), linked into cppgm++ only.
- `dev/src/toolchain/eh_table.cpp` (new): LSDA encode/decode (typed <->
  bytes), shared by writer and reader.
- `dev/src/toolchain/elf_object.cpp` (new): ET_REL writer.
- `dev/src/toolchain/elf_reader.cpp`: role + EH fact recovery.
- `dev/src/toolchain/object_module.*`: typed EH structs; CPGMOBJ1 encode /
  decode removed once the ELF path lands.
- `dev/src/toolchain/link_executable.cpp` / `runtime_library.cpp`: flat
  table synthesis, land/get-frame primitives, walker runtime, runtime-owned
  fundamental typeinfo definitions.
- `dev/src/toolchain/compile_unit.cpp`: typeinfo demotion, EH fact plumbing.

## Implementation findings

- Throw-payload windows with no calls inside are pruned after the
  region dataflow; otherwise throw-only objects grow dead abandon pads
  and an unexpected `_Unwind_Resume` reference (the cross-TU ref facts
  pin its absence).
- Runtime-owned typeinfo is synthesized at the object-module level
  (`AppendRuntimeTypeinfo` in runtime_library.cpp) rather than through
  lowering: the runtime module gains weak `_ZTI`/`_ZTS` items for all
  fundamental encodings plus `P`/`PK` variants, laid out per the
  Itanium records its own matcher reads. `IsRuntimeOwnedTypeinfoName`
  is the single owner of the demotion/provision name set.
- Pre-existing backend bug fixed en route: 9..16-byte by-value object
  returns dropped the second eightbyte (rdx) on both the callee return
  and both caller consumption paths. Nothing before
  `100-host-eh-compact-unwind-callee-save` returned a two-eightbyte
  class through a non-inlined call.
- The register allocator double-booked a callee-saved pool register
  when a scratch parameter's pending prologue copy materialized after
  a temp had already used and released the same register (observed
  while reshaping the runtime's `__cxa_throw`; the runtime kept the
  `cppgm_eh_record_of` helper as good factoring). Fixed in the audit
  turn: `resolve_location` hoists the copy into the prologue, so it
  now only targets registers no already-emitted code has written
  (`pool_clobbered_` tracking). Pinned pa28 fixtures are unchanged -
  they never exercised the corrupting overlap.

## Validation

1. Stage 1 (engine swap, private path only): implement backend landing pads
   + typed facts + private walker while `-c` still writes CPGMOBJ1; gate on
   `make test-report-through-pa30` (pa25/pa29 EH suites prove the new engine
   and walker end-to-end).
2. Stage 2 (host objects): ELF writer + driver switch + reader recovery +
   CPGMOBJ removal; gate on pa29 (object round-trip) and pa31 link/run.
3. Stage 3 (facts): diff `x.my.inspect` against `x.ref.inspect` for the 8
   fact-bearing tests; validate runtime behavior via the executed programs
   (host unwinder exercises CFI + LSDA for real); `readelf -wf`/gdb spot
   checks for CFI sanity.
4. Stage 4: typeinfo demotion + runtime definitions; re-run through-pa31.
5. Exit: `make test-report-through-pa31` clean, file audit clean, commits
   cohesive.

## Out of scope (later PAs)

`__cxa_rethrow` interop subtleties, foreign catch-all, virtual-base catches,
hosted-library EH, `new`/`delete` host mapping (still
`cppgm_builtin_operator_new` — first needed by pa32), Mach-O compact unwind
(no Mach-O target on this host).

## Architecture Review

- One EH codegen serves both link paths, exactly as planned: the region
  dataflow (`lowir_to_mir_eh.cpp`) annotates calls and publishes typed
  regions; `mir_to_native.cpp` records call-site byte ranges, landing
  offsets, and frame facts; `compile_unit.cpp` converts them to
  `EhFunctionInfo` on the `ObjectModule`. The ELF writer renders host
  sections from those facts, the ELF reader parses them back, and the
  private linker renders the same facts into its flat table. No path
  reconstructs semantic facts from strings or from emitted bytes other
  than at the declared ELF boundary.
- Pinned surfaces held: pa25/pa26/pa27 LowIR text is untouched (all
  PA31 work is below LowIR), pa28 strict MIR fixtures pass unchanged,
  and the pa29 object round-trip now flows through real ET_REL ELF.
- Ownership is single-homed: `IsRuntimeOwnedTypeinfoName` owns the
  demotion/provision set (demote in `compile_unit.cpp`, provide in
  `runtime_library.cpp`); `eh_table.cpp` owns LSDA bytes both ways;
  `frame_cfi.cpp` owns DWARF CFI knowledge; the reader/writer pair own
  the ELF layout constants as the two sides of one documented boundary.
- The audit found three real backend defects (the walker's
  abandoned-frame callee-save loss, the hoisted-copy double-booking,
  and the `LowerIndex` fast path dropping runtime counts) and one
  quadratic scan; all four are fixed (see pa31/audit.md
  Findings/Changes Made).

## Final Architecture Review

- Landing-pad frames now snapshot the full callee-saved pool set
  (`FinishFrame`): the private walker's landing contract (only
  rax/rdx/rbp installed, abandoned frames' spills lost) is sound
  because the landed frame's own epilogue restores its entry snapshot;
  the host unwinder sees the same snapshot through the CFI offset
  rules. This is the one deliberate cost of keeping a single codegen
  for both unwinders: five extra prologue stores per catch/cleanup
  frame, none for throw-only or non-EH functions.
- The hoisted prologue param copy (`resolve_location`) only targets
  never-clobbered registers (`pool_clobbered_`), closing the
  double-booking miscompile; the fallback (named frame home) already
  existed and is unchanged.
- The EH region dataflow resolves block labels through a map instead
  of per-merge linear scans, so the pass every function now runs stays
  linear-ish in block count.
- `LowerIndex`'s pinned-base fast path is gated on literal counts;
  runtime counts always take the general scale-in-rdx path (the fast
  path used to parse a temp count as literal 0 and drop the index -
  reachable only from hand-written LowIR, fixed regardless).
- Loud subset boundaries, verified not silently reachable: the LSDA
  action encoder's one-byte sleb profile (>63 distinct catch filters
  per function throws), item alignment caps, and the linux-only host
  object target. Each fails the compile loudly rather than degrading
  output.
- `make test-report-through-pa31` passes 2765/2765 with the file audit
  clean after all audit fixes; the fact-bearing pa31 tests pin the
  emission surface and the executed programs exercise the CFI/LSDA
  under the real host unwinder.
