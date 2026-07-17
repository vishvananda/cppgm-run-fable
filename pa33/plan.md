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
later hosted stages:

- SysV small-aggregate passing covers the all-INTEGER two-eightbyte
  class (`pass=gpr_pair`); SSE-classified eightbytes ({double,double}
  and mixed) stay on the memory path until a fixture exercises them.
- `__builtin_va_list` models the 24-byte cursor as `unsigned long[3]`;
  the `__va_list_tag` class spelling only matters for the mangling of
  va_list-typed C++ (non-extern-"C") signatures - a hosted-header
  (PA34+) concern.
- The -O1 call-site expansion covers constructors whose whole effect
  is literal member stores; a general inliner is later work.
- The pool-exhausted TLS store still stages through rax ahead of the
  wrapper call (a latent host-mode hazard no fixture reaches; the
  callee-saved staging path covers every tested shape).

## Validation

- Iterate per cluster with `make -C pa33 check TEST=tests/general/<t>.t`
  and `make test-report ACTIVE_TEST_REPORT_PAS='pa33'`.
- After each parser/sema/lowering/backend change, run the full
  `make test-report-through-pa33`; treat any older-stage failure as part of
  the change that caused it.
- Finish with `perl scripts/cppgm_file_audit.pl --stage pa33 --paths dev/src`
  and commit per cluster.
