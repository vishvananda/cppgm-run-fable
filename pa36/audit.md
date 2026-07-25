# PA36 Audit (loop 84)

## Audit Plan

Scope: the PA36 range `d5dd329c2..HEAD` (82 files, ~2.5k insertions) —
hosted header-emitted code linking/running through the host toolchain.
Suite state at audit start: 3340/3340 through-pa36, fileAudit pass.

Files to inspect, grouped by risk:

1. **EH and static-lifetime lowering** — `dev/src/lowering/lower_eh.cpp`,
   `dev/src/lowering/lower_static.cpp` (new, 308 lines),
   `dev/src/toolchain/eh_table.{cpp,h}`, `dev/src/x86/lowir_to_mir_eh.cpp`,
   `dev/src/toolchain/elf_reader.cpp`. Risks: LSDA spec-filter encode/decode
   asymmetry, fallback success paths around `__cxa_call_unexpected`,
   `__cxa_atexit` registration only working for fixture shapes.
2. **Symbol naming / ABI spelling** — `dev/src/lowering/lower_name.cpp`,
   `lower_name_std.cpp` (new, 170 lines), `lower_name_parts.h`,
   `lower_member.cpp`, `lower_unit.cpp`, `lower_global.cpp`. Risks: stringly
   semantic facts (already-mangled fragments threaded as strings, `#tmpl`
   marker keys), hard-coded libstdc++-private names the README forbids,
   duplicated ownership between sema and lowering for linkage decisions.
3. **Template machinery** — `sem_template.cpp`, `sem_template_check.cpp`,
   `sem_member_template.cpp`, `template_body.cpp`, `template_deduce.cpp`,
   `template_order.cpp`, `sem_trait.cpp`, `sem_lifetime.cpp`,
   `type_builder.{cpp,h}`. Risks: swallowed instantiation errors widened into
   fallback-success paths, retry loops that mask real failures (quadratic
   retry passes), extern-template suppression gated on source shape.
4. **Backend / host object path** — `lowir_to_mir_value.cpp`,
   `lowir_to_mir_flow.cpp`, `x86_encoding.{cpp,h}`, `mir_to_native.cpp`,
   `link_executable.cpp`, `elf_object.cpp`, `object_module.h`,
   `compile_unit.cpp`, `elf_program.cpp`. Risks: GOT-slot synthesis scans
   (quadratic over relocations), host call-ABI classification recomputed per
   call site, narrow spill compares regressing whole-program encodings.
5. **Core lowering** — `lower_convert.cpp`, `lower_vbase.cpp`,
   `lower_expr.cpp`, `lower_function.{cpp,h}` (LowerCallValue extraction),
   `lower_types.cpp`. Risks: mode-gated (`SeparateCompilation()`) forks
   drifting into duplicated logic, vbase adjustment fallbacks that silently
   use static offsets when the dynamic path is missing.

Cross-cutting checks:

- regression sweep: `make test-report-through-pa36` (all 36 stages);
- cheat sweep: env hooks, test-name matches, fixture-specific gates in
  `dev/src` (initial grep: clean);
- file-audit conformance: `perl scripts/cppgm_file_audit.pl --stage pa36
  --paths dev/src`, plus a check that no implementation moved into
  unchecked paths (`dev/*.cpp` outside `dev/src`, generated `.my` files);
- performance: look for per-call full-program walks, repeated
  `nm`/symbol-table rescans in the private linker, retry passes that
  re-instantiate the full deferred set each round.

## Findings

No blockers. The full-range review (every diff hunk read against the
current HEAD functions) found honest, mode-gated, typed implementations
throughout. Detailed dispositions:

**Integrity (cheat sweep) — clean.**
- No env hooks, test-name matches, or fixture-shape acceptance gates in
  `dev/src` (grep + read-through of every gate added in the range).
- `pa36/tests/`, `pa36/scripts/`, root `scripts/`, and the Makefiles are
  untouched in the range — no weakened checks; the `_Z1g` unscoped-variable
  spelling that `MangleVariableObjectName` now emits is pinned by the
  *unmodified handout oracle*
  (`700-hosted-pcrel-data-reloc-link-smoke.inspect.expect`), so the
  PA36 revert of the PA32 plain-name spelling follows the assignment
  contract (host TLS keeps the plain name for `_ZTW`/`_ZTH` interop).
- No dummy/stub outputs: unsupported shapes throw
  (`OutsideBoundary("function-local static with a destructor")` for the
  remaining array case, `"virtual-base adjustment lost its path"`,
  `"GOT reference reached private image layout"`, unencodable patch
  widths). Failure paths fail loudly.
- `lower_name_std.cpp` hard-codes only the Itanium-ABI-mandated 5.1.4.2
  abbreviation catalog (Ss/Si/So/Sd/Sa/Sb/St) with structural `::std`
  scope checks — that is the ABI spec, not a library-private name; the
  README's forbidden `std::__cxx11`/`std::__1` spellings appear nowhere.

**Fallback-success / swallowed errors — clean.**
- `RetryDeferredBodies` (sem_template_check.cpp): a still-failing body is
  caught and *not* re-appended (the only push site is the first-poison
  point, `AppendPoisonedBody`, sem_member_body.cpp:131), so the PA36
  retry/drain alternation terminates when no new bodies poison; a
  poisoned body demanded later still fails (`instantiation_error`
  rethrown at lowering). Pre-existing PA21 semantics, unchanged.
- Conditional noexcept (`EvaluateNoexceptSpec`) degrades conservatively
  to may-throw when the operand is dependent; instantiation recomposes
  with concrete types.
- LSDA spec filters: encode/decode are symmetric (slots above catch
  filters, `-(offset+1)` spec-area offsets, reader resolves indices back
  to symbols); the private link table ignores `EH_SPEC` records, which
  is the documented whole-program semantics (no unexpected machinery in
  the private runtime), not a dropped feature.

**Ownership / stringly facts — clean, two pre-existing minors noted.**
- New facts are typed state end to end: `throw_spec` as `vector<TypePtr>`
  (15.4p2-adjusted at composition), `asm_label` per overload slot,
  `linkage_name` for 7.1.3p9, `extern_class_scopes` as `Scope*` sets,
  `OP_GOT`/`EH_SPEC`/`AM_RIP`/`AM_GOT` enums, `member_template_body`,
  `LowGlobalInfo::extern_declared` (3.5p3).
- Mode gates are explicit typed parameters through single funnels:
  `LowerClassDirect/LowerAbiReturn(host_abi)`,
  `HiddenSignatureParams(separate_compilation)`,
  `imported_data_global` → `emit_global_address`/`global_mem_operand`
  (every backend global-address site routes through it).
- The `#tmpl` `MemberDefinitionKey` marker is built from a typed
  `ctor_template` bool; keys are opaque map identity, never parsed back.
- Minor (pre-existing, PA21): `LowGlobalInfo.object_name` uses a leading
  `@` as an in-band "low name is the object name" marker for weak local
  statics; PA36 taught `AppendTlsWrapperDeclares` about it. Confined to
  `LowerProgram` internals; left as-is with the convention documented at
  the consumer.
- Fixed (this audit): `MirProgram.host_tls` doubled as the host-object
  flag for the PA36 data-addressing model while the MIR-lowering layer
  used a separate `host_object` — two names for one fact set only
  together in `compile_unit.cpp`. Unified as `MirProgram.host_object`,
  set once inside `LowerLowIRProgramToMir` from its parameter.

**Regression risk against earlier assignments — clean.**
- Whole-program mode keeps every pinned shape: absolute addressing and
  encodings (`host_object=false` from lowir2native), PA27 vbase carrier
  params and trailing VTT, PA20 internal local-static guards, spec
  regions not armed, `ClassParamDirect` unchanged.
- The narrow-spill-compare guard (`FrameSizeOf(...) == cmp_width`) only
  *narrows* the direct-memory-compare case that previously read junk
  slot bytes; the reload path zero-extends (`MovzxMem` for 8/16-bit,
  implicit for 32-bit), which is correct for the boolean `cmp eq/ne`
  shapes that produce width-mismatched compares.
- `ClassParamDirectHost`'s `!is_polymorphic` term is redundant
  belt-and-braces (polymorphic classes cannot have trivial copy ctors),
  so no host-ABI divergence hides behind it.

**Conformance corners reviewed and accepted (not PA36 defects).**
- 14.8.2.1p4 base-DAG deduction takes the first unifying base rather
  than diagnosing two-base ambiguity — the same first-match semantics
  the pre-PA36 single-chain walk had, broadened to the DAG; likewise
  `ResolveExplicitInstantiationSpec` keeps the first candidate when
  partial ordering cannot order two matches. Ambiguity diagnosis is not
  demanded by any stage oracle.
- Host virtual-base parity is scoped to polymorphic classes (the plan
  documents this); non-polymorphic virtual-base classes keep the course
  carrier model consistently in signatures *and* bodies, and the
  static-offset arm of `AdjustPointerToVBase` applies only to them.
  The hosted-header surface that crosses the host boundary (iostreams)
  is polymorphic.
- Local-static guards do not use `__cxa_guard_acquire` (no thread-safe
  first-use init) — the PA20 single-threaded init model, pre-existing;
  PA36 only mirrored the guard's linkage/TLS-ness onto its object.

**Performance — clean.**
- GOT-slot synthesis (`link_executable.cpp`) is one pass over patches
  with a map-deduped slot per (symbol, addend) — no quadratic scan.
- The retry/drain alternation consumes and clears each queue per round;
  no full-deferred-set re-instantiation.
- LSDA spec lists dedupe by type-symbol list; ttype slots dedupe by
  symbol.
- `DemandExplicitMemberInstantiations` is O(records × deferred) but
  records are a handful per TU (pre-existing shape, extracted verbatim).

**File-audit conformance — clean.**
- fileAudit exit 0; the only warnings are pre-existing bad-division
  notes on headers untouched by this range.
- `lower_function.cpp` extractions (`lower_static.cpp`,
  `LowerCallValue`, `ConvertMemberPointerValue`,
  `DemandExplicitMemberInstantiations`, `RegisterGlobal`) are verbatim
  moves into audited `.cpp` files registered in
  `frontend_source_sets.mk` — no logic hidden in headers or unchecked
  paths, no forked copies.

## Changes Made

1. **Single owner for the host-object fact** — renamed
   `MirProgram.host_tls` → `host_object` and set it inside
   `LowerLowIRProgramToMir` from the existing `host_object` parameter;
   dropped the separate assignment in `compile_unit.cpp`
   (`dev/src/mir_model.h`, `dev/src/x86/lowir_to_mir_program.cpp`,
   `dev/src/x86/mir_to_native.cpp`, `dev/src/toolchain/compile_unit.cpp`).
   The encoder's rip-relative/GOT and TLS-wrapper gates now read the
   same fact the MIR lowering was given, eliminating the aliased pair
   of flags that had to be set in tandem.
2. **LSDA decoder rejects unterminated spec lists** — the 1024-entry
   safety bound in `DecodeEhTable` previously fell out of the loop
   silently, truncating a corrupt table into an accepted one; it now
   returns decode failure (`dev/src/toolchain/eh_table.cpp`).

## Validation

- Build: `make build` clean (pre-existing warnings only).
- fileAudit: `perl scripts/cppgm_file_audit.pl --stage pa36 --paths
  dev/src` exit 0 after the changes.
- Tests: `make test-report-through-pa36` — 3340/3340, all 36 stages
  (log: `/tmp/pa36-audit-test.log`).
