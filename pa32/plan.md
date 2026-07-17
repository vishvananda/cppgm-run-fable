# PA32 Plan: Host Object/Toolchain Interoperability

## Goal

`cppgm++ -c` objects must be ordinary host-linkable ELF relocatables: correct
symbol surface (locals, weak/COMDAT, TLS), host-ABI symbol spelling, and
host-runtime allocation symbols, while the PA29 own-link path and all earlier
stage fixtures keep working.

Baseline at plan time: 31 pa32 tests fail; everything through pa31 passes.

## Failure clusters and owners

1. **Local symbols missing** (anon-class-static-helper-closure,
   anon-namespace-special-member-symbols): internal-linkage definitions get no
   symtab entries. Owner: `toolchain/elf_object.cpp` (+ the module model must
   carry the mangled spelling of internal symbols; today
   `ObjectSymbol::external_name` is cleared for `SB_INTERNAL`).
2. **No COMDAT groups / wrong vague linkage** (class-template-member-duplicate,
   complex-template-owner-duplicate, function-template-duplicate,
   inline-header-duplicate, host-operator-prefix-template-owner-symbol,
   inline-static-member-outdef-header-duplicate,
   odr-mergeable-template-default-parameter): weak definitions must move into
   per-symbol sections inside SHT_GROUP/GRP_COMDAT groups signed by the symbol;
   the out-of-class inline static member definition must be `binding=weak` in
   lowering. Owners: `toolchain/elf_object.cpp` for the section/group shape,
   `lowering/lower_*.cpp` for the weak-binding fact.
3. **Own-runtime allocation names leak into host objects** (the virtual-base
   lifecycle tests, host-template-ptr-ref-vtable-symbols,
   anon-namespace-implicit-special-member-linkage): calls reference
   `cppgm_builtin_operator_new/delete`. The implicit (and replaceable) global
   allocation functions are `_Znwm`, `_Znam`, `_ZdlPv`, `_ZdaPv` in the host
   ABI. Owner: `lowering/lower_unit.cpp` for the object names;
   `toolchain/runtime_library.cpp` keeps the own-link path working by
   providing/aliasing the Itanium spellings.
4. **extern "C" semantics** (extern-c-variable-import, host-f64-call-arg-shuffle,
   extern-c-overload-collision-bad, extern-c-static-redecl-bad): an unbraced
   linkage-specification declaration is treated as if it contains `extern`
   (7.5p7), so `extern "C" int extv;` must not define; two functions with the
   same C language linkage name must be diagnosed (7.5p6), as must an
   extern-"C"-then-`static` linkage mismatch. Owner: `sema/` declaration
   binding.
5. **Mangling gaps** (synthetic-std-hard-substitution,
   synthetic-std-inline-namespace, namespace-operator-template-std-string,
   dependent-alias-return-decltype-operator, dependent-nttp-default-return,
   odr-mergeable-template-default-parameter): missing `St`/`Sa`/`Sb`/`Ss`…
   abbreviations, `NSt3__1…E` (St as nested-name prefix + inline namespace),
   and `Tn <type>` template-argument spelling for NTTPs with
   instantiation-dependent declared type. Owner: `abi/abi_mangle_encode.cpp`
   (PA30 layer) and the semantic facts feeding it.
6. **TLS host surface** (thread-local-variable-export/import): today TLS
   variables are plain `.data` with a fake `_Z4extv` object name and accesses
   fall back to direct addressing. Host surface required by the tests:
   - variable object name is the real mangling (`extv`), STT_TLS in
     `.tdata`/`.tbss`;
   - every TU that uses the variable defines the weak COMDAT wrapper
     `_ZTW<enc>` and reaches the storage through it;
   - the wrapper tests the weak-undefined `_ZTH<enc>` init hook and uses
     local-exec `R_X86_64_TPOFF32` access (what the inspect surface pins).
   Owners: `lowering/lower_global.cpp` (names), `x86/mir_native_data.cpp` /
   `x86/mir_to_native.cpp` (TLS items + wrapper synthesis + tls_addr = call
   wrapper), `toolchain/elf_object.cpp` (.tdata/.tbss, STT_TLS, TPOFF32,
   weak-undef binding).
   Pinned constraints: pa28 strict MIR refs keep `tls_addr r11, @x__tls_wrapper`
   and the direct-global native fallback for LowIR that only declares the
   wrapper; pa13/pa15/pa18 refs pin the C++→LowIR shape for qualified TLS
   names, so only the unqualified-global spelling changes (`_Z4extv`→`extv`).
   The wrapper body is synthesized on the object-emission path so neither the
   LowIR nor the serialized MIR shape moves.
   Boundary: defining-TU `_ZTH` emission for dynamically initialized TLS is
   wired only when the guarded `__tls_init` helper exists; dynamic-init TLS
   interop beyond that is PA33+ (the tested subset is constant-init).
7. **Trivial special members over-emitted**
   (trivial-default-constructor-symbol-pruned): a trivial default constructor
   must not produce an object symbol or calls. Owner: lowering (suppress
   definition + call emission for trivial implicit special members).
8. **Singles**:
   - anonymous-storage-member-init-runtime-smoke: overload-resolution bug
     ("no matching function") — sema.
   - member-function-pointer-abi-runtime-smoke: "unsupported i128 load form" —
     x86 backend wide-value path.
   - host-eh-goto-out-of-try: goto leaving a try scope must run the local
     cleanup exactly once — lowering EH/scope-exit dataflow.

## Design decisions

- The mangled spelling of an internal-linkage definition travels on
  `ObjectSymbol` as a display/local name; ELF emission renders LOCAL
  `STT_FUNC/STT_OBJECT` entries. Relocations keep going through section
  symbols, so linking behavior is unchanged.
- COMDAT: each weak definition item becomes its own `.text.<sym>` /
  `.data.<sym>` (`.tbss`-style dispatch by item kind) section, wrapped in an
  SHT_GROUP section with GRP_COMDAT signed by the weak symbol. FDEs for
  grouped functions relocate against the grouped section's section symbol so
  linkers drop them with the group. The own-toolchain reader/linker keeps
  working because resolution stays by symbol name.
- Allocation functions: lowering assigns the Itanium spellings; the own
  runtime library exports those spellings (aliases of the existing
  implementations) so PA24–31 own-link tests keep passing.
- TLS: LowIR keeps `storage=thread_local` + declared wrapper with `tls_for`;
  the object path synthesizes the wrapper body (LE access + `_ZTH` probe) and
  places TLS data in `.tdata`/`.tbss`. The lowir2native executable path keeps
  the single-threaded direct-global model (no fixture churn; no own-link test
  uses TLS).
- Diagnostics for extern "C" collisions live in sema at declaration-merge
  time, using the declared language linkage on the entity, not name-string
  probing.

## Outcome notes

All eight clusters landed; `make test-report-through-pa32` is clean and
the file audit passes. Design points settled during implementation:

- Mode boundaries: three host-parity behaviors apply on the
  separate-compilation path only, because whole-program fixtures pin
  the reference presentation: the Itanium allocation-function
  spellings, the odr-use demand for elided effect-free destructor
  invocations, and the trivial-default-construction pruning (the last
  additionally restricted to instantiated specializations - the PA31
  object-fact fixtures pin the explicit call for non-template code).
- TLS: LowIR and MIR shapes are unchanged (pa13/pa18/pa28 fixtures pin
  them). The `-c` path sets `MirProgram::host_tls`; the native encoder
  then routes every `tls_addr` through the per-TU `_ZTW` wrapper and
  synthesizes its body (guarded-init call when the module defines one,
  else the weak `_ZTH` probe, then local-exec `R_X86_64_TPOFF32`
  access). The ELF writer adds `.tdata` (SHF_TLS), STT_TLS symbols
  (defined and undefined), and weak-undefined binding for the probe.
  Boundaries: dynamic-init TLS interop relies on the module-local
  guarded init only (no `_ZTH` export yet), and the wrapper carries no
  CFI/FDE - both PA33+ work.
- Mangler: alias transparency expands through a written-argument
  binding frame; `Tn` applies to function-template arguments whose
  NTTP's written declared type mentions the template's own parameters;
  deferred dependent-typed defaults keep their written literal
  spelling. `Sa`/`Sb`/`Ss`/`Si`/`So`/`Sd` abbreviations apply to
  direct members of `::std`; abbreviation-as-prefix (e.g. `_ZNSaIcE…`)
  is out of scope until hosted headers (PA34).
- Deduction: an elided default whose declared parameter type needs
  instantiation-time facts defers as a marked `deferred_default`
  argument - deduction treats only those as non-deduced value slots so
  partial-specialization SFINAE shapes keep failing structurally.
- Latent fixes surfaced by newly-running fixtures: register-class
  object slots passed by value now load their container bytes (caller
  and callee agreed on content-in-register everywhere else), and i128
  loads/stores accept global operands.

## Validation

- Per-cluster: `make -C pa32 check TEST=tests/general/<case>.t` plus targeted
  nm/readelf inspection matching the checked-in inspect plans.
- After each cluster lands: `make test-report ACTIVE_TEST_REPORT_PAS='pa32'`.
- After lowering/backend/mangler changes: full `make test-report-through-pa32`
  (the required gate) — pa13/pa15/pa18/pa28 pin LowIR/MIR shapes, pa30 pins
  mangling facts, pa29/pa31 pin the own-link path.
- `perl scripts/cppgm_file_audit.pl --stage pa32 --paths dev/src` before
  finishing.
