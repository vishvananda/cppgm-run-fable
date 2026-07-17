# PA32 Audit

## Audit Plan

Scope: commits `295440c79..HEAD` (7 PA32 commits), reviewed against
`pa32/plan.md` and the assignment README. All 2857 tests and the pa32
file audit pass at audit start; this pass looks for cheating shapes,
ownership problems, and performance risks rather than failures.

Files to inspect, by risk:

1. **Mode gates** — `lowering/lower_unit.cpp`, `lower_function.cpp`,
   `lower_program.h`: the three `SeparateCompilation()`-gated behaviors
   (Itanium allocation spellings, elided-dtor odr-use demand, trivial
   default-construction pruning). Verify the gates select between two
   real presentations pinned by fixtures, not between "implemented" and
   "skipped", and that the facts they consume are semantic, not string
   probes.
2. **TLS object surface** — `x86/mir_to_native.cpp`,
   `x86/mir_native_data.cpp`, `toolchain/elf_object.cpp`,
   `toolchain/compile_unit.cpp`: `_ZTW` wrapper synthesis happens on the
   object-emission path. Verify the wrapper is generated from the
   module's own facts (not a byte-templated stub), STT_TLS/`.tdata`/
   `.tbss`/TPOFF32 emission is real, and `host_tls` gating does not
   disable required work elsewhere.
3. **Mangler additions** — `lowering/lower_name.cpp`,
   `lower_name_parts.h`, `lower_name_template.cpp`: `St`/`Sa`/`Ss`…
   abbreviations, `Tn` NTTP spelling, alias transparency. Verify
   abbreviations key off the semantic owner scope (direct member of
   `::std`), `Tn` keys off the written declared type's parameter
   references, and substitution-table order stays host-compatible.
   Watch for stringly matching against full demangled text.
4. **COMDAT / local symbols** — `toolchain/elf_object.cpp`,
   `toolchain/compile_unit.cpp`, `toolchain/object_module.h`: SHT_GROUP
   with GRP_COMDAT, per-symbol sections, FDE relocation retargeting,
   LOCAL symtab entries. Verify group membership is derived from symbol
   binding facts, and the own-toolchain reader path is not special-cased
   by test shape.
5. **extern "C" semantics** — `sema/decl_binder.cpp`, `scope.h`,
   `decl_function.cpp`, `ast*`: 7.5p6/p7 rules. Verify diagnostics use
   declared language linkage on entities, not name strings.
6. **goto scope exits** — `lowering/lower_function.cpp`/`.h`,
   `lower_eh.cpp`: `ScanLabelContexts` pre-scan. Verify it runs once per
   function (not per goto), and cleanup/EH depth math is structural.
7. **Deduction / defaults** — `sema/template_args.cpp`,
   `template_deduce.cpp`, `type.h`: `deferred_default` flag. Verify the
   flag is set from declared-type dependence, and deduction changes are
   scoped to those slots only.
8. **Backend singles** — `x86/lowir_to_mir_wide.cpp` (i128 global
   operands), `x86/lowir_to_mir_flow.cpp`: check no fallback-success or
   silently-narrowed paths.
9. **Allocation-function surface** — `lowering/lower_unit.cpp`,
   `toolchain/runtime_library.cpp`: Itanium spellings on the `-c` path,
   own-runtime aliases on the own-link path. Verify aliases are real
   definitions, not resolution hacks.

Performance risks to check:

- `ScanLabelContexts` recursion cost and trigger conditions (should be
  gated on the function actually containing labels/gotos).
- `DemandElidedDtorUses` unit walk (once per unit, not per symbol).
- ELF emission: per-symbol section/group construction should be linear
  in symbols; watch for per-symbol rescans of the item list, and
  quadratic name lookups in symtab/section-name handling.
- Mangler substitution table lookups (linear scans acceptable at these
  name sizes; flag anything super-linear per component).

Ownership boundaries to check:

- Mangled spellings must be produced once (lowering/ABI layer) and
  carried on LowIR/ObjectSymbol; ELF/toolchain code must not re-derive
  or parse names (no downstream recovery of semantic facts).
- TLS wrapper naming: the `__tls_wrapper` LowIR name to `_ZTW` host
  name mapping should happen in one place.
- extern "C" facts should live on the sema entity, not be re-inferred
  in lowering.

File-audit issues to check:

- The "audit splits" commit moved code from `lower_function.cpp` and
  `sem_binder.cpp` into `lower_eh.cpp`, `lower_vtable.cpp`, and
  `sem_special.cpp`. Verify the moved code lands in files that own the
  concern (cohesion, not size-dodging), headers gained declarations
  only, and nothing moved to a path outside `--paths dev/src`.

## Findings

No cheating shapes, skipped phases, or regressions were found. What
the inspection established, by plan area:

1. **Mode gates are presentation switches, not skip gates.** All three
   `SeparateCompilation()` behaviors do strictly more host-correct
   work on the `-c` path (Itanium allocation spellings, demanding
   odr-used elided dtors, pruning genuinely no-op trivial default
   constructions) while the whole-program branch keeps the complete
   fixture-pinned shape. `TrivialDefaultConstruction` is
   fact-driven (implicit ctor, no user ctors, no construction work in
   the subobject tree); its restriction to instantiated
   specializations is pinned by the handout PA31 object-fact fixtures,
   which expect the explicit call for non-template classes.
2. **TLS wrappers are compiled artifacts, not canned payloads.**
   `EncodeTlsWrapperItem` assembles the body from module facts:
   guarded-init call when the module defines `<global>__tls_init`,
   otherwise a weak `_ZTH` probe, then a local-exec `R_X86_64_TPOFF32`
   access against the actual global's label. Wrapper emission is
   demand-driven (defined-or-used), binding (weak/internal) follows
   the declared LowIR binding, and undefined TLS references carry
   STT_TLS. All TLS data lands in `.tdata`; a separate `.tbss` is a
   file-size optimization the host linker does not require - accepted.
3. **ELF COMDAT emission is real and typed.** Weak definitions move to
   per-symbol sections inside SHT_GROUP/GRP_COMDAT signed by the weak
   symbol; items also carrying a strong name stay in plain sections;
   FDEs relocate against the member section's symbol so groups discard
   atomically; `.rela` headers of members carry SHF_GROUP and are
   listed in the group payload. A planning cross-check throws if the
   precomputed symtab index drifts from the built table.
4. **Mangling keys off semantic/written facts.** `Sa`/`Sb`/`Ss`/`Si`/
   `So`/`Sd` require the entity to be a direct member of global
   `::std` (scope-pointer checks, not name text); `Tn` fires when the
   NTTP's written declared type mentions the template's own parameter
   names (AST scan with shadowing semantics); alias transparency
   expands through `AliasFrame` binding frames with the enclosing
   context saved/restored RAII-style; deferred dependent-typed
   defaults re-resolve at concrete instantiation, so the deferral
   `catch` cannot convert an error into silent success.
5. **extern "C" facts live on entities.** 7.5p6 uses per-overload-slot
   `fn_c_linkage` flags; 7.5p7 uses a parser-recorded `linkage_single`
   fact consumed by the directly-contained declaration only; the
   static-after-extern diagnostic (7.1.1p7) compares recorded binding
   facts. No name-string probing.
6. **goto scope exits are structural.** `ScanLabelContexts` runs once
   per function body (linear pre-scan mirroring the lowering's scope
   pushes); each goto then closes exactly the crossed cleanup scopes
   and EH contexts (`__cxa_end_catch` for handler contexts) in O(depth)
   and rejects jumps into try/handler regions.
7. **Backend singles are real fixes.** i128 loads/stores gained global
   operand forms (address materialization + paired halves) and still
   throw on genuinely unsupported shapes; the register-class
   by-value object argument fix loads container bytes instead of
   passing a stray address.
8. **File-audit splits are cohesive.** `DemandElidedDtorUses`/
   `TrivialDefaultConstruction` moved next to the other program-level
   demand logic in `lower_vtable.cpp`, goto/EH lowering into
   `lower_eh.cpp`, `BindAnonymousUnionMembers` into `sem_special.cpp`
   (class-completion machinery). Headers gained declarations only; the
   six `bad-division` warnings predate PA32 and none of the moved code
   left `dev/src`.

Three avoidable scan costs were found and fixed (below):

- `ElfObjectWriter::AppendGroupHeaders` scanned every `.rela` source
  for every COMDAT group - O(groups x rela sections), and COMDAT count
  scales with template instantiations.
- `BuildObjectModule` rescanned all module symbols once per TLS
  wrapper label and once per weak-undefined label.
- `EncodeMirProgramModule` rescanned all globals, all functions, and
  the full label-name list for every TLS wrapper candidate.

Accepted as fine: `TrivialDefaultConstruction`'s linear scan over
`units_` (one unit on the `-c` path; whole-program callers are gated
off before it), and the once-per-function `ScanLabelContexts` walk
(linear, comparable to the existing epilogue pre-scan).

## Changes Made

- `dev/src/toolchain/elf_object.cpp`: `SectionData` records its
  `.rela` header index when `BuildSectionHeaders` plans the table;
  group payloads read it directly instead of scanning `rela_sources`
  per group (the `rela_sources`/`rela_base` parameters dropped from
  `AppendGroupHeaders`).
- `dev/src/toolchain/compile_unit.cpp`: the TLS wrapper and
  weak-undefined marking is one pass over `module.symbols` with set
  membership on the label lists, replacing the per-label rescans.
- `dev/src/x86/mir_to_native.cpp`: the wrapper-synthesis loop builds
  the global-name, function-name, and used-label sets once, replacing
  three full scans per wrapper.

No functional behavior changed; all three are scan-shape cleanups.

## Validation

- `make -C pa32 test`: 78/78 pass on the cleaned tree.
- `make test-report-through-pa32`: ALL TESTS PASSED (2843/2843),
  exit 0 - every stage pa1..pa32 green, confirming no regression in
  the pinned LowIR/MIR/mangling/own-link surfaces.
- `perl scripts/cppgm_file_audit.pl --stage pa32 --paths dev/src`:
  pass with the six pre-existing `bad-division` warnings (all predate
  PA32; PA32 added declarations only to the warned headers).
