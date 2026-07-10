# PA25 Plan — advanced-language slice to LowIR

PA25 extends the PA14–PA24 `cppgm++ --emit-lowir` path with capturing lambdas,
`std::initializer_list` interop, RTTI/`typeid`, `dynamic_cast`, and
source-level exception lowering. The checked-in `tests/general/*.ref` files are
the oracle (relaxed comparison: metadata groups, `alias object` lines, and
`@function` names are canonicalized; instruction text, block labels, slot names
and `%t` numbering are compared exactly).

## Existing foundation (reused, not rebuilt)

- RTTI-lite records from PA17: `lowering/lower_vtable.cpp` already emits
  `@__rtti_class_X` / `@__typeinfo_name__class_X` (`_ZTI*`/`_ZTS*`) and the
  vtable head slots `[i64 offset-to-top][ptr rtti]` with vptr = vtable+16.
- Cleanup-only unwind regions from PA24: `FunctionLowerer::OpenEhRegion` /
  `CloseEhRegion` (`lowering/lower_member.cpp`) already emit
  `eh_try ^call_unwind_dispatch_N` / `eh_end` / dispatch blocks running armed
  dtor cleanups + `resume`, with dispatch-block reuse keyed on the armed set.
- Unwind analysis: `NodeMayThrow`, `fn_unwind_no` (sema) → `unwind=no`
  function metadata.
- Lambda hybrid model from PA24: `sema/sem_lambda.cpp` synthesizes closure
  classes with sema-laid-out capture fields (`LambdaCapture{binding, name,
  offset, referee, is_this}`), `SN_CLOSURE_INIT` lowered by
  `LowerClosureInit`. By-ref and `this` captures work; by-copy is rejected.
- LowIR EH instruction vocabulary is already specified (pa13/lowir.md PA22
  section) and parsed by the comparator: `eh_try`, `eh_cleanup`, `eh_end`,
  `eh_catch`, `exception`, `exception_selector`, `resume`.

## Ownership boundaries

- **sema** owns: which constructs are accepted (typeid requires a declared
  `std::type_info`; capture legality; initializer_list recognition and
  overload preference), typed semantic nodes for each new construct, capture
  field layout, closure class identity, and may-throw facts.
- **lowering** owns: RTTI global rendering (extended from lower_vtable's
  class records to fundamentals/pointers), unwind-region placement, catch
  dispatch shape, dynamic_cast control flow, initializer_list backing-array
  materialization. Lowering consumes typed sema facts (class layouts, capture
  offsets, vslot indices, rtti keys) — never re-derives them from text.
- New runtime interfaces are *declared* externals only
  (`__cxa_*`, `__dynamic_cast`, `_Unwind_Resume`, `__gxx_personality_v0`);
  no runtime code is generated beyond calls into them.

## Phases (each ends with scoped report + through check + commit)

### 1. Hidden-EH region alignment (10 LowIR-mismatch tests)

Refs show three gating rules our emitter must match:
- A call inside a full-expression that owns ≥1 nontrivial-dtor temporary is
  wrapped in an `eh_try` region even when the callee is `unwind=no` and even
  when the armed cleanup set is empty (dispatch = bare `resume`).
- Aggregate member-init constructor calls are wrapped under the armed-locals
  rule (dispatch covers enclosing scope locals only; partially-initialized
  member subobjects are *not* added — simplified course model).
- Compiler-generated runtime-helper calls (`__cxa_bad_typeid`, `__dynamic_cast`
  etc.) are never wrapped; scope-exit dtor calls are not wrapped (dtors are
  implicitly `unwind=no`).
- Call results consumed across region boundaries materialize through
  `$call__N` slots (visible in refs).
- `_Unwind_Resume` / `__gxx_personality_v0` declares appear only when a
  `__cxa_*` function is referenced, not for cleanup-only units (pa24 refs
  stay eh-free because nothing there may throw; keep that invariant).

### 2. typeid / RTTI extensions

- Sema: `typeid(type-id)` / `typeid(expr)` produce a const `std::type_info`
  lvalue tied to an RTTI key; error if `std::type_info` is not declared as a
  class in namespace std (200-typeid-requires-*-bad tests). cv/ref stripped
  from the operand type. `type_info::operator==`/`!=` calls with typeid
  operands lower to `cmp eq/ne ptr` on RTTI global addresses (no call).
- Non-polymorphic operands: static `addr @__rtti_<key>`.
- Polymorphic glvalue operands: null-check → `^typeid_fail_N` block calling
  `__cxa_bad_typeid` + zero-value return, `^typeid_scan_N` loads vptr,
  `index i8 vptr, -8`, loads the rtti pointer.
- New RTTI record kinds in lower_vtable: fundamentals
  (`@__rtti_int`, `__fundamental_type_info`), pointers (`@__rtti_type_Pi` =
  `{vt+16, name, i32 0, pointee}`, `__pointer_type_info`); template type
  parameters resolve through the substituted type; no eager class completion
  for pointer-to-class operands (100-typeid-template-pointer-no-eager-complete).
- Lambda closure classes get `_ZTIZ…EUl…E_` manglings via the closure class
  record.

### 3. dynamic_cast (pointer + reference forms)

Lower to: `$dyn_cast__N` slot, store null, null-check branch;
`^dyn_cast_scan`: `call ptr @__external_runtime____dynamic_cast(val, src_rtti,
dst_rtti, 0)`; pointer form: fail = null result; reference form: fail block
calls `__cxa_bad_cast`. Static upcasts keep the existing base-subobject path.

### 4. Source throw / try / catch

- Bind `SK_THROW`/`SK_TRY` in `SemBinder::BindStatement`; typed nodes carry
  the thrown type / handler list (types or ellipsis, optional named binding).
- throw: `__cxa_allocate_exception(size)` → store/copy-construct payload →
  `__cxa_throw(exc, @rtti, dtor-or-0)`; also emits a weak `@__ehobj_<key>`
  zero global (ref scaffolding). `throw;` → `__cxa_rethrow()`. Both are
  noreturn; block terminates with zero-value return.
- try/catch shape per ref: `eh_try ^catch_dispatch_N` around body,
  dispatch block with one `eh_catch @rtti, sel` per handler
  (catch-all uses the catch-all marker), `^catch_entry`: `exception ptr` +
  `exception_selector i32` + cmp/branch chain, `^catch_body`:
  `__cxa_begin_catch`, optional `$catch__N` store + by-value copy into the
  named handler variable, `eh_cleanup ^catch_cleanup` (end_catch + eh_end +
  resume), `^catch_next`: resume, `^try_end` continuation.
- EH runtime declares with `role=` metadata exactly as in refs.

### 5. Capturing lambdas (by-copy, defaults, mutable, class objects)

- sema: accept `LC_COPY` / `[=]` / `mutable` / pack captures where the copied
  type is in the supported subset. Copy captures produce *value* fields laid
  out by the class layout machinery (offsets remain sema-owned facts).
  `mutable` drops the const on `operator()`.
- lowering: `LowerClosureInit` stores scalars / calls the copy constructor
  for class captures into the field offsets; by-ref/this behavior unchanged.

### 6. std::initializer_list

- sema: recognize `std::initializer_list<T>` (even from a forward
  declaration) as a builtin 16-byte `{__begin ptr, __size i64}` record;
  braced-init-list → init-list conversion in overload resolution (preferred
  over per-element ctors), auto deduction, range-for support via the
  `__begin`/`__size` fields; non-class element types only.
- lowering: materialize backing array `$initlist__N : obj<sizeof(T)*K x
  align>` element stores + `$argobj__N` `{ptr, K}` at the use site.

## Validation

- Fast loop: `make test-report ACTIVE_TEST_REPORT_PAS='pa25'` and
  `make -C pa25 check TEST=tests/general/<case>.t`.
- After each phase: root `make test-report-through-pa25` (regressions in
  pa14–pa24 shapes are blockers — especially phase 1, which touches shared
  emission).
- File audit: `perl scripts/cppgm_file_audit.pl --stage pa25 --paths dev/src`.
- Manual sanity (optional): feed generated LowIR into pa28 lowir2native.

## Architecture Review

How the implementation actually landed, phase by phase:

- **EH regions / source EH** — one region machine in `FunctionLowerer`
  (`lower_member.cpp`): `OpenSegmentRegion`/`BeginFullExpression` decide
  up-front from `ScanArmsCleanups`/`SegmentContainsCall` evaluation-order
  scans; `OpenEhRegion` keys dispatch-block reuse on the rendered
  cleanup-content signature (`CleanupSignature`), and a dispatch inside a
  try re-arms the try's handler set and routes to its entry. Source-level
  throw/try/catch statement lowering, catch-handler chains, and the
  typeid/dynamic_cast expression forms live in `lower_eh.cpp` (a
  responsibility split registered in `frontend_source_sets.mk` — the
  conditional-value/branch forms moved with them because they open
  per-arm segment regions). Sema owns the statement binding
  (`BindThrowStatement`/`BindTryStatement`) and may-throw facts
  (`NodeMayThrow`, `SemTreeMayThrow` know `SN_THROW`).
- **typeid / RTTI** — sema recognizes `std::type_info` once
  (`FindStdTypeInfo`/`StdTypeInfoEntity`), records the entity on the
  `SemUnit`, and types the query nodes (`SemNode::typeid_operand`,
  `typeid_dynamic`). `LowerProgram` renders records on first use, deduped
  by Itanium encoding: class records via the PA17 chain, fundamentals /
  pointers / never-completed specializations via `RttiTypeRef`;
  `ThrowRttiRef` resolves fundamentals to the C++ runtime's strong
  records. The operator==/!= fold checks the sema-recognized entity by
  pointer identity (`IsStdTypeInfo`) and compares record addresses — no
  runtime call.
- **dynamic_cast** — sema gates on real polymorphism facts
  (`is_polymorphic`, `BaseClassDistance`) and lowers to a pre-nulled
  slot, null-check, one `__dynamic_cast` runtime call, and (reference
  form) a `__cxa_bad_cast` fail block. No new IR operations.
- **Capturing lambdas** — the PA24 hybrid model extended: sema lays out
  by-copy value fields through the ordinary class-layout machinery
  (offsets are `ClassField` facts; captured-this is the typed
  `ClassField::captured_this`/`SemNode::captured_this` pair), enforces
  5.1.2p16 constness for non-mutable by-copy reads, and numbers closures
  per enclosing function body for the Itanium `<lambda-sig>`
  discriminator. Lowering stores scalars / copy-constructs class captures
  into the sema offsets. Closure manglings assemble from structured parts
  (enclosing-function encoding + `Ul...E<disc>_` signature) through a
  shared substitution table — no string surgery; `::main` prefixes spell
  bare `4main` (g++ parity).
- **initializer_list** — recognition is one structural predicate
  (`IsStdInitializerListTemplate`/`IsStdInitializerList`: the template
  named initializer_list declared directly in `::std`), used by overload
  preference, auto deduction, braced deduction, range-for, and the
  builtin-record build. A program that only declares the template gets a
  real `ClassInfo` built through `BeginClassLayout`/`LayoutField`/
  `FinishClassLayout` with implicit special members, so list values copy
  like any trivial class. Lowering materializes the backing array with
  per-element stores and fills `{begin, size}` from the record's own
  field offsets.

Ownership held: sema owns acceptance, layouts, may-throw and identity
facts; lowering consumes typed facts and owns rendering. Runtime
interfaces are declared externals only.

## Final Architecture Review

Post-audit state (see `audit.md` for the full findings):

- The audit fixed eight issues: duplicate weak closure symbols from
  block-scoped discriminators, the `_ZZ4mainv` prefix, two `"__this"`
  name-string gates in lowering (one produced wrong code), missing
  const on non-mutable by-copy captures, a hardcoded initializer_list
  size-field offset, lowering-side re-recognition of `std::type_info`
  by scope-name strings, and a scope-unchecked `initializer_list`
  match in braced deduction; plus the builtin record now declares its
  implicit special members.
- Remaining known boundaries are contract-level, not defects:
  class-element initializer_lists work for the trivially-destructible
  subset the shipped tests pin (element destruction is PA26+ object-model
  work; user-dtor elements are README-declared UB), `dynamic_cast<void*>`
  and reference-form corner cases stay deferred to PA26 per the stage
  handoff, and EH/RTTI LowIR is text-contract-only until the later
  host-EH assignments (`pa28/README.md` defers the private
  exception/runtime ABI path; the non-EH slice executes correctly through
  the reference `lowir2native`).
- No interpreter/VM/payload substitutes, no test-shaped gates, no
  comparator or fixture edits, no file-audit bypasses (the four header
  warnings are the `;`-count heuristic on declaration growth; all
  PA25 header additions are declarations). fileAudit passes;
  `make test-report-through-pa25` passes 2430/2430.
