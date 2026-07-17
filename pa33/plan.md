# PA33 Plan: Host C++ ABI/Runtime Behavior

## Goal

Once host link succeeds, the linked program must behave correctly under the
ordinary x86_64 Linux host C++ ABI/runtime: virtual dispatch and vtable
ownership, RTTI, covariant return adjustment, the richer rethrow/cleanup/
noexcept EH subset, host varargs, and the host ABI manglings those surfaces
make observable. PA32 objects already link; PA33 makes the runtime behavior
and remaining symbol spellings part of the contract.

Baseline at plan time: 24 pa33 tests fail; everything through pa32 passes.

## Failure clusters and owners

1. **Varargs builtins** (va-list-start, va-arg-gp-pointer):
   `__builtin_va_list` (SysV: array-of-1 struct {gp_offset, fp_offset,
   overflow_arg_area, reg_save_area}), `__builtin_va_start/va_end/va_arg`,
   and a register save area + spill prologue for `...` functions. Owners:
   `sema/sem_builtin.cpp`/type system for the builtin type and expressions,
   `lowering/lower_expr.cpp` + `lowering/lower_function.cpp` for the save
   area and va_start/va_arg lowering, x86 backend only if a new LowIR shape
   is needed (prefer plain loads/stores on the va_list struct).
2. **__builtin_alloca** (builtin-alloca-link-smoke): dynamic stack
   allocation. Owners: sema builtin declaration; lowering emits a dynamic
   stack-adjust op; `x86/` implements it (sub rsp, align 16, value = rsp).
3. **__decay(T) builtin transform** (builtin-transform-alias-mangling):
   parse the `__decay(T)` builtin transform type, evaluate it in alias
   substitution, and keep alias transparency in the mangling (function
   mangles by the alias-expanded dependent type as written). Owners:
   parse/sema type parsing + `abi/abi_mangle_encode.cpp`.
4. **abi_tag** (abi-tag-copy-ctor-base-alias, abi-tag-dtor-base-alias):
   parse `__attribute__((__abi_tag__("tag")))` on special members and spell
   `C1B3tag`/`D1B3tag` etc. Owners: parse attribute plumbing → entity fact →
   `abi/abi_mangle_names.cpp`.
5. **[[no_unique_address]]** (empty-no-unique-address-copy-assign): accept
   the standard attribute on non-static data members; empty-class member
   takes no storage (EBO-style overlap) and implicit copy assignment calls
   member operator= and skips the empty member. Owners: parse member
   attributes, `sema/class_info.cpp` layout, `sema/sem_special.cpp` implicit
   assign synthesis.
6. **False vbase ambiguity** (forwarded-reference-vbase-condition,
   virtual-base-return-condition): converting to / looking up in a virtual
   base reachable through more than one path reports "ambiguous base class
   subobject". A virtual base is one shared subobject; path dedup belongs in
   `sema/sem_bases.cpp` subobject enumeration.
7. **Overload/init sema bugs** (eh-lambda-template-owner-cleanup: no
   matching constructor for Guard<Fn>(Fn&&) with closure Fn;
   by-address-aggregate-param-lifetime: "no conversion for member
   initializer" in a nested-class + array-member aggregate). Owners:
   `sema/sem_ctor.cpp` / `sema/init.cpp` — diagnose by reduction first.
8. **TLS dynamic-init/backend crash** (thread-local-wrapper-access):
   "gpr_read on unhoused operand" ICE in Guard::Guard reading a TLS
   pointer; also exercises TLS class objects with ctor/dtor (dynamic init
   was left at the PA32 boundary). Owners: `x86/lowir_to_mir*` for the ICE;
   `lowering/lower_global.cpp` + object path for `__tls_init`/`_ZTH` if the
   test's dynamic-init surface needs it.
9. **Dependent-name manglings** (dependent-nontype-expression,
   dependent-enable-if-alias-expression, dependent-owner-prefix-substitution,
   dependent-ratio-and-fn): function template signatures must mangle as
   written after alias expansion: `N <prefix> <unqualified> E` dependent
   typename with prefix substitution (`NS_2IdIT_E4typeE`), `X <expr> E`
   template arguments, `sr <type> <name>` qualified dependent expressions,
   decltype/declval call expressions, and `Tn <type>` NTTP heads already
   started in PA32. Owner: `abi/abi_mangle_encode.cpp` fed by richer
   semantic facts from `lowering/lower_name*.cpp` — keep one mangler layer,
   no second name builder in object code.
10. **Nested-lambda substitution** (nested-lambda-parameter-substitution):
    `_ZZZ…ENKUlvE_clEvENKUlR5ScopeE_clES2_` — the local-scope encodings must
    enter the substitution table so the trailing parameter reuses `S2_`.
    Owner: `abi/abi_mangle_encode.cpp` substitution bookkeeping for
    local/lambda scopes.
11. **Template-template parameters** (template-template-parameter-mangling):
    `use<box>` spells the TT param as `T_IiE` in the signature and `I3boxE`
    in the argument list. Owner: mangler + the semantic fact that the
    parameter type is a TT-param specialization.
12. **-O1 inline-ctor pruning** (o1-simple-inline-ctor-pruned): at -O1 a
    simple inline ctor call must be inlined so the linkonce C1/C2 symbols
    are not emitted. PA32 already prunes unreferenced ctors; the gap is the
    call-site inlining (or direct-init strength reduction) that removes the
    last reference at -O1. Owner: lowering/lowiropt inline-and-prune path.
13. **EH cleanup correctness** (eh-rethrow-cleans-outer-local,
    eh-unmatched-call-cleanup): rethrow (`_Unwind_Resume`/`__cxa_rethrow`
    bookkeeping) must run enclosing-scope cleanups when the rethrown
    exception leaves the function, and a non-matching handler's frame must
    still run its cleanup chain during phase 2. Owners:
    `lowering/lower_eh.cpp` region/action tables + landing-pad emission.
14. **Covariant return adjustment** (self-covariant-return-adjustment):
    Derived::self() overriding Base::self() with Derived at a nonzero Base
    offset needs the covariant thunk to adjust the returned pointer, not
    just `this`. Owners: `lowering/lower_vtable.cpp` thunk synthesis.
15. **Small-aggregate by-value ABI** (small-aggregate-by-value): passing
    {long,long} by value to a host-compiled callee must use the SysV
    two-INTEGER-register classification. Owner: x86 call lowering
    classification for by-value class arguments in SeparateCompilation mode.
16. **eh-noexcept-terminate**: compiles standalone; reproduce through the
    pa33 harness to find which step exits 1 (likely a later phase — link or
    inspect), then classify.

## Ownership boundaries

- Semantic facts (attributes, dependent types, TT params, vbase paths) are
  computed in `sema/` and carried on entities; the mangler consumes typed
  facts, never re-parses text.
- All host-ABI spelling stays in the PA30 `abi/` layer; the object writer
  keeps preserving raw names.
- EH tables and thunks are lowering-owned; the x86 backend only gets new
  ops where LowIR cannot express the shape (alloca, va-arg register save).
- Mode gating: host-only behaviors keep gating on `SeparateCompilation()`
  per the PA32 convention so pa13–pa29 fixtures pinning whole-program
  shapes do not move.

## Outcome and remaining boundaries

All 24 baseline failures are fixed; `make test-report-through-pa33` is
clean (2917/2917) and the pa33 file audit passes. Boundaries left for
later hosted stages (each is scope, not deferred breakage - see
`audit.md` for what the audit fixed instead of deferring):

- SysV small-aggregate passing covers the all-INTEGER two-eightbyte
  class (`pass=gpr_pair`); SSE-classified eightbytes ({double,double}
  and mixed) stay on the memory path until a fixture exercises them
  (own-to-own calls stay self-consistent; only an untested
  our-object/host-object by-value crossing would notice).
- `__builtin_va_list` models the 24-byte cursor as `unsigned long[3]`;
  the `__va_list_tag` class spelling only matters for the mangling of
  va_list-typed C++ (non-extern-"C") signatures - a hosted-header
  (PA34+) concern. `__builtin_va_start` does not validate its
  last-named-parameter argument (it evaluates and is ignored, matching
  the cursor model); a non-va_list `va_arg` operand is rejected.
- The -O1 call-site expansion covers constructors whose whole effect
  is literal member stores; a general inliner is later work.
- `abi_tag` applies to the special members the stage tests (C1/C2/D1/
  D2 spellings); tags on ordinary functions, variables, and types
  parse and are dropped by their own declaration - they never
  contaminate other symbols - until a later stage makes those
  spellings observable.
- `[[no_unique_address]]` is consumed in the leading member position
  (the fixture and primary position); the after-declarator spelling
  parses and is dropped. Overlap conflict probes cover nua-vs-nua,
  nua-vs-member, and nua-vs-base-tree same-type collisions; empty
  bases keep the pre-existing shared-offset convention for non-nua
  code, and tail-padding reuse for non-empty nua members is later
  work.
- TLS dynamic init is per-variable (guarded `_ZTW` wrappers, matching
  the course reference); the TU-wide `__tls_init`/`_ZTH` trigger of
  [basic.start.dynamic]p5 - first odr-use of one TLS variable running
  the whole TU's dynamic inits - is a hosted-runtime (PA34+) surface
  no fixture exercises.
- A closure type re-encountered inside its own enclosing-function
  encoding re-spells in full rather than compressing to `S<n>_`; no
  fixture (or host link) reaches that shape, and the compressed form
  is not derivable without a host oracle.

## Architecture Review

Where each PA33 fact lives, from the audit pass:

- **Semantic facts are typed and single-owner.** abi_tag rides the
  AST declarator being built (never parser-global state), then
  `ClassCtor.abi_tags`/`dtor_abi_tags`; `[[no_unique_address]]` rides
  `ClassField`; TT-param/dependent-name/alias facts ride the
  written AST and `TemplateInfo`. The mangler consumes typed facts
  only - the audit found no demangled-text probing, no fixture-shaped
  gates, and no second name builder in object code (the PA30
  `abimangle` tool's encoder is a separate fact-file-driven program,
  not linked into the compiler).
- **Mode gating follows the PA32 convention.** Host-only behaviors
  (gpr_pair classification, -O1 ctor expansion, noexcept terminate
  regions, EH pad slice partitioning, guard-first TLS init) gate on
  `SeparateCompilation()`; whole-program LowIR/MIR shapes pinned by
  pa13-pa29 fixtures are byte-stable.
- **EH ownership.** Region arming and pad routing are lowering-owned
  (`lower_eh.cpp`/`lower_member.cpp`); the x86 layer derives static
  region facts by dataflow (`lowir_to_mir_eh.cpp`) and the toolchain
  builds LSDA chains from the region parent tree. In-frame handler
  routing is by code jumps; `resume` is reserved for leaving the
  frame (or chaining cleanup pads whose coverage the dataflow
  proves). The `eh_end` frame-record pops in catch pads are
  whole-program bookkeeping and are suppressed on the host path,
  where they would corrupt the pad's dataflow coverage.
- **Backend boundaries.** LowIR grew only role-annotated calls
  (`va_start`, `alloca`) plus the `gpr_pair` pass annotation - shapes
  LowIR cannot express - all validated by `lowir_validate.cpp`. The
  x86 layer owns the register save area, cursor seeding, dynamic
  stack adjust, and the SysV AL count. TLS address materialization is
  a call boundary in the backend's register discipline (rax alias
  invalidation, callee-saved or spilled staging).
- **File organization.** The mangler is four cohesive units:
  `lower_name.cpp` (scope/type spellings), `lower_name_template.cpp`
  (written dependent forms + alias transparency),
  `lower_name_signature.cpp` (function-template signature assembly),
  `lower_name_local.cpp` (5.1.7 local entities), sharing internals
  through `lower_name_parts.h` declarations only.

## Final Architecture Review

Post-audit state: the audit removed the one latent wrong-code boundary
the plan had documented (TLS rax staging across the wrapper call) and
three undocumented ones it found (noexcept unwind loop, handler-local
rethrow double-destruction, unmatched-catch enclosing-local leak - the
last matching the course reference but not the host runtime), plus
parser-state abi_tag contamination and unconditional nua overlap. Each
fix was verified against host g++ behavior directly and the full
2917-test suite. No interpreter/VM/trampoline/templated-binary
substitutes exist anywhere in the stage; every artifact is compiled
from the module's own facts. Remaining boundaries (above) are scope
choices with no silent-wrong-code path inside the tested contract:
unsupported shapes either compile correctly by other means, reject
loudly (`OutsideBoundary`, virtual-base covariant returns, non-va_list
cursors), or sit outside what any host link in the suite can observe.

## Validation

- Iterate per cluster with `make -C pa33 check TEST=tests/general/<t>.t`
  and `make test-report ACTIVE_TEST_REPORT_PAS='pa33'`.
- After each parser/sema/lowering/backend change, run the full
  `make test-report-through-pa33`; treat any older-stage failure as part of
  the change that caused it.
- Finish with `perl scripts/cppgm_file_audit.pl --stage pa33 --paths dev/src`
  and commit per cluster.
