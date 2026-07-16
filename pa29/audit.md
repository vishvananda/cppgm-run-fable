# PA29 Audit

## Audit Plan

Scope: the twelve PA29 commits `a611d8160..1ccdc5301` — the new
`dev/src/toolchain/` module (compile pipeline, object format, linker,
ELF reader, runtime library), the x86 backend EH/relocatable-encoding
work (`mir_to_native.*`, `mir_native_data.*`, `lowir_to_mir_wide.cpp`,
`lowir_to_mir_flow.cpp`), the lowering additions (`lower_eh.cpp`,
`lower_arg_bind.cpp`, `lower_vtable.cpp`, separate-compilation mode),
the driver rewrite in `dev/cppgm++.cpp`, and the shared front-end
edits (preprocess `-I`, parser/sema additions for function-try-blocks,
128-bit ints, statement expressions).

Files to inspect:

- `dev/cppgm++.cpp` — option parsing and mode orchestration only per
  the plan's ownership table; no fallback success paths, no
  test-shape gates, errors exit failure.
- `dev/src/toolchain/compile_unit.cpp` — the per-TU pipeline must run
  all phases (1-7, parse, sema, lower, LowIR round-trip through
  `ParseLowIRProgram`+`Validate`, PA28 MIR lowering, native encoding);
  no phase skips, no cached/copied artifacts.
- `dev/src/toolchain/object_module.cpp` — object read/write must be a
  faithful serialization; check for hidden payloads (embedded LowIR or
  interpreter dispatch instead of encoded x86).
- `dev/src/toolchain/link_executable.cpp` — symbol resolution policy
  (strong/weak/internal), startup synthesis, `__cppgm_unwind_raise`;
  verify no name-list or test-shape gating; runtime library compiled
  on demand only when names remain unresolved, through the ordinary
  pipeline; error paths (duplicate strong, unresolved, missing main)
  real. Performance: resolution should be map-based, not
  quadratic-over-symbols per module.
- `dev/src/toolchain/elf_reader.cpp` — a real ET_REL reader (sections,
  symtab, rela) with bounds checks; no fixture-specific relocation
  hacks.
- `dev/src/toolchain/runtime_library.cpp` — must be C++ *source text*
  compiled at link time by this compiler, not pre-encoded bytes or a
  host-built blob; check the ABI entry points are real implementations
  (exception arena, RTTI match walk, dynamic_cast) rather than stubs
  that satisfy only checked-in tests.
- `dev/src/x86/mir_to_native.cpp` + `mir_native_data.h/.cpp` — the
  relocatable-encoding split; verify `mir_native_data` is a real
  module boundary (data-image encoding) and not a file-audit fragment;
  EH record build/pop sequences must be genuine encodings.
- `dev/src/x86/lowir_to_mir_flow.cpp` / `_value.cpp` / `_program.cpp`
  — EH conservative mode must be driven by typed facts (function
  contains EH ops), not string sniffing; PA28 discipline preserved for
  non-EH functions (strict fixtures pin this).
- `dev/src/x86/lowir_to_mir_wide.cpp` — 128-bit lowering; check it is
  real code generation (add/adc, mul decomposition, shifts) and that
  the file split is a real boundary.
- `dev/src/lowering/lower_eh.cpp`, `lower_arg_bind.cpp`,
  `lower_vtable.cpp`, `lower_expr.cpp` — the arg-bind split must be a
  real ownership boundary; EH lowering (function-try-blocks, by-value
  class catch, ctor unwind) must lower through typed AST/sema facts.
- `dev/src/lowir/lowir_parser.cpp` / `lowir_validate.cpp` — additive
  extensions only (`eh_catch @rtti`, `exception_selector`,
  `object_root`, `object=@self`); earlier-stage accept/reject behavior
  unchanged.
- Front-end edits (`preprocess.*`, `ast_parse_*`, `sem_*`) — verify
  additive; `-I` search must not change PA1-PA9 behavior.

Performance risks to inspect:

- linker symbol resolution: per-reference linear scans over all
  modules/symbols; patch fixups re-walking all items per symbol;
- object serialization doing byte-at-a-time I/O over large images;
- `compile_unit` re-running lowering or emission more than once per
  TU (e.g., emit text, parse, then re-emit);
- runtime library recompiled more than once per link, or compiled
  when no unresolved names remain;
- EH conservative mode leaking into non-EH functions (would slow all
  code and break PA28 strict fixtures);
- ELF reader quadratic section/symbol lookups.

Ownership boundaries to verify (from pa29/plan.md):

- driver = option parsing/orchestration only; toolchain owns symbol
  resolution policy; x86 owns encoding and knows labels/patches but
  never external names; lowering owns mangling; lowir owns the text
  contract. Look for external-name policy leaking into `dev/src/x86/`
  or object-format knowledge leaking into the driver.
- Single-owner facts: symbol binding, entry/init/fini roles, and TLS
  wrapper mapping should each be represented once (typed fields in
  `ObjectModule`), not re-derived by string parsing downstream.

File-audit issues to inspect:

- `b739d2dae` ("file-audit splits: lower_arg_bind, sem_builtin"),
  `e34097132` (mir_native_data split), `eae4013d7` ("audit trims after
  the 128-bit merge") — each split must be a coherent module, not a
  hidden fragment; "trims" must not have deleted substance (weakened
  checks, dropped validation) to fit size limits.
- Run `perl scripts/cppgm_file_audit.pl --stage pa29 --paths dev/src`
  and confirm no path is excluded or renamed to dodge the audit.

Validation plan: `perl scripts/cppgm_file_audit.pl --stage pa29
--paths dev/src` and `make test-report-through-pa29` after any fixes;
spot-check PA28 strict fixtures unaffected.

## Findings

Verified miscompiles (each reproduced natively against a g++ oracle
before fixing):

1. **i128 staging clobbered live parameter registers.** Wide (i128)
   lowering stages pairs through rax:rdx with rcx/rsi/r10/r11
   scratch, but `LowerInstruction`'s wide early-return skipped the
   `arg_homes_clobbered_` marking and `ParamUseIsForwardable` did not
   treat wide instructions as clobbers, so parameters stayed
   register-forwarded across them. Repro returned 0 instead of 42.
2. **Ctor function-try unwind cleanups double-fired or misfired.**
   The unmatched-selector path re-destroyed armed subobjects already
   destroyed on the region-dispatch edge (double destruction), and
   its statically captured cleanup list destroyed members whose
   construction never ran (first-initializer-throws case).
3. **Nested-try routes into a fn-try lost the outer try.** The
   region dispatch's balancing `eh_end` popped the fn-try frame
   record whose markers never ran on that route, so the fn-try's
   handlers were skipped entirely; inner catch_next routes also
   entered the handler chain without destroying armed subobjects
   (15.2p2).
4. **By-value catch parameters destroyed twice on implicit rethrow**
   (once inline, once in the armed catch cleanup the rethrow unwinds
   through).
5. **Destructor function-try-blocks had try-statement semantics
   only**: no implicit rethrow (15.3p15), and the member/base
   epilogue ran after the handlers instead of before handler entry
   (15.2p11). A ctor-initializer on a non-ctor function-try was
   silently ignored.
6. **Unwind out of a scope with destructible locals leaked them.**
   `throw`/`throw;` never wrapped in a dispatch region when only
   scope cleanups deeper than the innermost EH context were armed
   (no landing edge runs those), and returns out of a destructor
   skipped the member/base epilogue entirely.
7. **Dynamic class-global initializers silently zero-filled**
   unrecognized initializer shapes (e.g. a comma expression
   `S g = (f(), S(9));` produced a zero object).

Fallback and ownership findings:

8. The driver owned the on-demand runtime-library decision (plan
   assigns all symbol-resolution policy to the linker), and its
   pre-link check ran before the support module existed, so
   cleanup-only programs compiled the runtime library needlessly
   (contradicting the comment in link_executable.cpp).
9. `-l` search and object-format classification lived in the driver
   instead of the toolchain.
10. Member-pointer thunks: the adjusted-signature fallback matcher
    ignored this-cv (could pick a const overload's slot), and
    class-valued parameters/results produced a plausible-looking but
    ABI-mismatched forwarding thunk instead of an error.
11. `LowerMemberAssignment`'s zero-fill branch dereferenced
    `children[0]` of a synthesized non-member lhs (out-of-bounds);
    value-position statement expressions without a tail produced an
    empty splice instead of a desync error.
12. The LowIR validator accepted `object=@anything` while the object
    reader ignored the text after `@` (a stringly fact nobody
    checked); eh_catch selectors accepted 0/negative values although
    0 is the runtime's no-match sentinel.
13. The compiler object reader did not validate entry/init/fini
    symbol indices (malformed file -> out-of-bounds in the linker)
    or item alignment; the ELF reader accepted arbitrary alignment
    fields.
14. The 128-bit builtin-typedef fallback was copy-pasted in
    decl_binder.cpp and sem_lifetime.cpp.
15. File-audit hygiene: lower_arg_bind.cpp was an overflow fragment
    (four unrelated features; charter comment omitted the division
    expansion; lower_function.h still attributed its functions to
    lower_expr.cpp).
16. x86 loose ends: the MIR serializer threw on EH functions
    (missing opcode cases), MI_THROW/MI_RESUME and
    call_returns_noreturn were dead, labeled imm64 patches carried
    the default TRUNC kind, label-less self-rewrite patches bloated
    object patch lists, data-image encoding grew byte-at-a-time
    without reserve, and EH-marker/throw/resume runtime calls were
    invisible to the call-crossing analysis (xmm values could stay
    register-resident across `__cppgm_eh_match` in hand-written
    LowIR).
17. Performance nits: per-alias linear scans over the label space in
    BuildObjectModule; LinkExecutable deep-copied every input module
    and every image item.

Reviewed and accepted as-is:

- The runtime library is genuine C++ source compiled through the
  full pipeline at link time (real RTTI walk, exception arena,
  cross-cast dynamic_cast) - not a stub, template, or copied
  runtime. `__builtin_nanl` drops a non-empty payload tag (documented
  deviation; the default quiet NaN is produced).
- The re-landing protocol (an unmatched selector resumes and lands
  on the still-armed outer record, which re-runs its markers) is the
  *reference compiler's own emission shape* - the checked-in
  PA25-27 `.ref` LowIR pins it - so it stays; the fixes above repair
  only the PA29-new paths (function-try, ctor cleanups) where no ref
  exists, keeping every ref-pinned shape byte-identical.
- `eh_cleanup` without a block operand is the intentional bare
  marker form CloseEhRegion emits inside dispatch blocks.
- Member-pointer slot lookup returning "not virtual" for a total
  miss stays: ClassInfo has no per-method virtual flag, the vslots
  table is the virtualness registry, and a genuine virtual member
  whose typed facts miss both matchers would desync everywhere else
  first. The strengthened matcher plus the class-ABI boundary error
  bound the risk.
- i128 arguments pass in a 16-byte stack container rather than the
  SysV GPR pair: internally consistent across our own objects;
  `__int128` never crosses the tested `extern "C"` interop subset
  (the plan's ABI note now records this).
- The VTT-shape additions for virtual direct bases are unconditional
  because they are correctness fixes validated by native execution;
  no earlier-stage ref covers such a class.

## Changes Made

Commits (in order):

1. `Audit PA29: linker-owned runtime policy, object hardening, i128
   clobber fix` - findings 1, 8, 9, 13, 17: RuntimeModuleSupplier
   callback moves the on-demand policy into LinkExecutable (inputs
   move in by value; hook externalization extracted as a named
   helper); LoadObjectModuleFile/FindLibraryObject in the toolchain;
   role-symbol/alignment validation in both readers; defined-symbol
   map in BuildObjectModule; wide ops clobber argument registers.
2. `Audit PA29: function-try and unwind-cleanup correctness` -
   findings 2-6: fn-try swap window covers EmitCatchNext; inner
   catch_next routes destroy armed subobjects and keep the outer
   record armed under two-plus enclosing tries
   (RouteKeepsOuterTryArmed); rethrow-path catch-param destruction
   belongs to the armed cleanup; dtor fn-try binds function_try with
   the epilogue inside the try region, armed at entry and retired as
   it runs; returns emit the not-yet-run epilogue
   (EmitEhReturnUnwind); throw/rethrow wrap over
   HaveCleanupsAboveEhBoundary; ctor-initializer misuse errors.
3. `Audit PA29: loud lowering boundaries, strict LowIR facts,
   one-owner helpers` - findings 7, 10-14: comma-shaped class-global
   initializers lower with elision, unknown shapes throw; pm-thunk
   matcher this-cv check and class-ABI boundary errors;
   OP_ASS non-member lhs fix; stmt-expr desync errors; object=@X
   self-name validation; positive selectors; ResolveBuiltinTypeName;
   sem_lambda_state.h extraction (size-limit fix with a real
   boundary).
4. `Audit PA29: real module boundaries for the arg-bind/expansion
   split` - finding 15: lower_arg_bind.cpp = argument binding
   (LowerCallArgument/MaterializeClassArg move in);
   lower_expand.cpp = compiler-expanded forms (builtin folds,
   statement expressions, wide div/mod); header attributions fixed.
5. `Audit PA29: x86 backend consistency trims` - finding 16 and the
   remaining nits.

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa29 --paths dev/src`:
  pass (6 pre-existing bad-division warnings, no fatals).
- `make test-report-through-pa29`: 2682/2682 across pa1-pa29,
  including the PA28 strict suite (34/34) that pins the register
  allocator and the PA13-27 LowIR refs that pin the front end -
  every EH fix is gated on PA29-new facts so ref text is unchanged.
- Fifteen native repro programs (double/missing destruction, dtor
  fn-try rethrow and epilogue order, nested-try routing, unwind
  local cleanup, early-return destructors, comma-initialized class
  globals, i128 parameter clobber and div/mod identities,
  member-pointer virtual dispatch) each match their g++ oracle
  (-std=c++03 for the dtor-noexcept cases, matching this
  implementation's dialect).
- Negative checks: `object=@wrong` and `eh_catch @t, 0` now fail
  validation; ctor-initializers on non-ctor function-try bodies are
  rejected at bind time; an EH-function machine-IR dump serializes.
