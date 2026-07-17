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

## Architecture Review

- Naming has one owner per layer, as planned. Sema records the facts
  (declared language linkage per overload slot, `fn_declared_inline`,
  elided-dtor odr-uses, `anonymous_storage` field rows); lowering
  turns them into ABI spellings exactly once (`lower_name*.cpp`,
  `FunctionEntry` for the four replaceable allocation functions,
  `AppendTlsWrapperDeclares` for `_ZTW`); the toolchain preserves the
  spelling it was given. The only downstream name derivation is the
  `_ZTW`→`_ZTH` probe spelling at wrapper-synthesis time, which is the
  ABI-defined mechanical prefix swap on an already-mangled encoding.
- The mode boundary is one bit with one meaning. The three
  `SeparateCompilation()`-gated behaviors (Itanium allocation
  spellings, elided-dtor demand, trivial-default-construction pruning)
  choose between two complete presentations - the host-parity object
  surface and the fixture-pinned whole-program shape - never between
  implemented and skipped work. `host_tls` is the same boundary one
  layer down: the `-c` path always routes `tls_addr` through the
  synthesized per-TU wrapper, while the private executable model keeps
  its pinned single-threaded shape.
- The runtime library now defines the replaceable allocation functions
  as ordinary C++ `operator new/delete` definitions, so their object
  names flow through the same `FunctionEntry` decision as every call
  site; no string of either spelling exists outside that one function.
- ELF emission consumes typed module facts only: COMDAT membership
  from `ObjectSymbol` bindings, TLS placement from
  `ImageItem::is_thread_local`, LOCAL names from
  `ObjectSymbol::local_name`, weak-undefined probes from an explicit
  flag. Nothing in the writer parses names or re-derives semantics.
- Mangler additions are written-form/AST-driven: `Tn` from the NTTP's
  written declared type mentioning the template's own parameters,
  alias transparency through a scoped expansion frame with real
  shadowing rules, `St`/`Sa`/`Ss`-family abbreviations keyed on the
  semantic owner scope (direct member of global `::std`), not on name
  text.
- Pinned surfaces held: pa13/pa15/pa18 LowIR and pa28 strict MIR
  fixtures are byte-identical (TLS wrapper bodies synthesize below
  MIR), pa30 mangling fixtures pass, and the pa29/pa31 own-link path
  is unchanged apart from the shared allocation-function spelling.

## Final Architecture Review

The audit (pa32/audit.md) confirmed the plan's shape survived
implementation and found no skipped phases, fallback success paths,
fixture-shape gates, or ownership splits. Three avoidable scan costs
were cleaned up: the SHT_GROUP payload builder scanned every .rela
source per group (now an O(1) `rela_index` recorded on the section),
and the TLS wrapper paths in `compile_unit.cpp` / `mir_to_native.cpp`
rescanned full module lists per wrapper (now one pass with set
lookups). Accepted boundaries, all documented in Outcome notes: all
TLS data lands in `.tdata` (zero-filled bytes instead of a separate
`.tbss`, semantically equivalent for the host linker), dynamic-init
TLS interop beyond the module-local guarded init is PA33+, the
wrapper carries no CFI/FDE yet, and abbreviation-as-prefix mangling
waits for hosted headers (PA34). `make test-report-through-pa32` and
the pa32 file audit pass on the final tree.

A follow-up audit pass chased a `pa3/tests/300-triple.t` timeout that
appeared on a loaded machine after the audit landed. It was not a
pa32 regression: the shared phase 1-2 representation
(`TranslatedChar`) spent 24 bytes per source code point and grew
without a reserve (~0.4GB peak on the 12MB stress input), the four
early-stage tool mains read stdin one character at a time through the
synced streambuf, and the PA3 calculator re-grew its per-line token
vector from zero capacity on each of 492k lines. All three were fixed
at their owners (`source_translation`, a shared `tool_stdin.h` reader
used by the tool entry points, `ctrl_expr`); outputs stay
byte-identical and the run dropped from 3.6s/432MB to 1.83s/168MB
(reference: 1.32s/9.8MB), restoring comfortable headroom against the
10s text-test budget. Details in pa32/audit.md.

## Validation

- Per-cluster: `make -C pa32 check TEST=tests/general/<case>.t` plus targeted
  nm/readelf inspection matching the checked-in inspect plans.
- After each cluster lands: `make test-report ACTIVE_TEST_REPORT_PAS='pa32'`.
- After lowering/backend/mangler changes: full `make test-report-through-pa32`
  (the required gate) — pa13/pa15/pa18/pa28 pin LowIR/MIR shapes, pa30 pins
  mangling facts, pa29/pa31 pin the own-link path.
- `perl scripts/cppgm_file_audit.pl --stage pa32 --paths dev/src` before
  finishing.
