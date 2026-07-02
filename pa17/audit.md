# PA17 Audit

## Audit Plan

Scope: the PA17 polymorphism slice — the single commit `81adf4bcd` ("PA17:
virtual functions, vtables, dynamic dispatch, RTTI-lite emission") on top of
the PA16 audit commit (`d3ec4abd7`), under `dev/src`.
Baseline at audit start: `make test-report-through-pa17` green (1192/1192,
pa17 22/22), `cppgm_file_audit.pl --stage pa17 --paths dev/src` passes with
the one pre-existing `parser.h` warning untouched since PA6.

### Files to inspect

Sema (new):
- `sema/sem_virtual.cpp` (253) — slot recording, 10.3p2 override matching,
  10.3p7 covariant returns, override/final checks, key-function selection,
  vpointer-store node factory

Sema (grown):
- `sema/class_info.h/.cpp` — `VirtualSlot`/polymorphic facts, vpointer
  layout reservation, triviality/effects queries keyed off `is_polymorphic`
- `sema/sem_class.cpp` (1446, near the 1500 cap) — body pre-scan, base slot
  inheritance, key-function anchoring, dtor vpointer stores
- `sema/sem_ctor.cpp`, `sem_special.cpp` — vpointer-store actions in user
  and synthesized ctor/dtor bodies, polymorphic storage-prefix suppression
- `sema/sem_member.cpp`, `sem_expr.cpp/.h` — dispatch classification,
  qualified-call suppression
- `sema/sem_new.cpp` — deleting-slot dispatch marking on `delete`
- `sema/decl_binder.cpp/.h`, `sem_binder.cpp/.h` — `OnFunctionDeclared`
  widening (specs/declarator/pure), pure-specifier recognition
- `sema/type_builder.cpp/.h` — `override`/`final` recording

Lowering (new):
- `lowering/lower_vtable.cpp` (196) — vtable/RTTI/typeinfo registry and
  rendering, strong/weak/declare resolution, `__cxa_pure_virtual`

Lowering (grown):
- `lower_expr.cpp` — indirect dispatch at slot-marked callees
- `lower_function.cpp/.h`, `lower_member.cpp` — `SN_VPOINTER_STORE`,
  deleting epilogue, indirect-call signature helper, base-reference
  projection for locals
- `lower_new.cpp` — deleting-slot dispatch in `LowerDelete`
- `lower_unit.cpp` — D0 entries, complete-entry co-demand, key-function
  registration, vtable/function fixpoint in `Write()`
- `lower_program.h`, `lower_name.cpp/.h` — vtable state, `_ZTV/_ZTI/_ZTS`
  manglers

### What I will look for

- Regressions against PA16-and-earlier outputs (monotonicity: polymorphic
  behavior must key off `is_polymorphic`/slot facts only).
- Dummy/stub artifacts: vtables with fabricated slots, dispatch that
  actually calls direct, vpointer stores without a demandable vtable.
- Test-shaped gates: acceptance keyed to test names, source shapes, or
  fixture-specific spellings rather than semantic facts.
- Stringly semantic facts: class-key/display parsing, name-string dispatch
  identity, lowering re-deriving facts sema owns (e.g. `ClassKeyString`
  splitting `NamedTypeInfo::display`; the deleting epilogue's
  `operator delete` lookup).
- Ownership boundaries from plan.md: slot metadata on `ClassInfo` only;
  override checking in `sem_virtual.cpp`; vtable rendering on
  `LowerProgram`; dispatch decided in sema (`vtable_slot`), not rediscovered
  in lowering.
- Boundary coverage: the README boundary lists "explicit base qualification
  suppressing virtual dispatch for supported calls" — verify both the
  implicit-this form (`Base::f()` inside a member) and the member-access
  form (`d.Base::f()` / `p->Base::f()`) behave; check pure-specifier and
  virtual-dtor edge declarations fail cleanly when outside the subset.
- Performance: per-call slot search cost, per-class body pre-scan, the
  vtable/function fixpoint in `Write()`, whole-registry walks in
  `AddUnit`, slot-vector copies at base binding.
- File audit: new code in focused new files; `sem_class.cpp` headroom under
  the 1500-line cap; no implementation moved to unchecked paths.

### Method

Read the full diff file by file against plan.md's ownership boundaries and
the README's Assignment Boundary / Out Of Scope lists; cross-check emission
shapes against checked-in refs (`400-header-out-of-class-virtual-vtable`,
`400-key-function-vtable-without-local-construction`, pure/covariant spec
cases); verify hook ordering (`fn_inline_def` stamping vs
`OnFunctionDeclared`); exercise gaps with scratch inputs through the built
compiler; fix blockers; rerun the pa17 suite, the full
`test-report-through-pa17`, and the file audit.

## Findings

### Blockers (all fixed)

1. **Scalar `delete` skipped virtual dispatch — and all destruction — when
   the static class's destruction chain was effect-free** (`sem_new.cpp`).
   The dispatch marking sat inside the `NeedsDestruction(*cls)` gate, which
   keys on user-provided destructors anywhere in the subobject tree. A
   polymorphic class with `virtual ~B() = default;` fails that gate, so
   `delete p` (p of static type `B*`, dynamic type `D` with a user
   destructor) lowered to a bare `call void @operator_delete(%p)`: no
   destructor call, no deleting-slot dispatch, `D::~D()` never runs.
   5.3.5p3 requires dispatch through the virtual destructor regardless of
   the static chain's triviality. Verified with a scratch input before the
   fix (direct `operator_delete`) and after (vpointer load, slot 1 load,
   indirect call).

2. **PA17 turned PA16's virt-specifier rejection into silent acceptance
   outside class bodies** (`decl_binder.cpp`, `sem_class.cpp`). PA16's
   type builder threw `OutsideBoundary("virt-specifier")` on any
   `override`/`final`; PA17 records them and checks their meaning only when
   a class body is open, and nothing had ever checked the `virtual`
   decl-specifier's placement. Verified accepts-invalid before the fix:
   `int f() override {}` at namespace scope, `int Base::f() override {}`
   out of class, block-scope `virtual int g();`, out-of-class
   `virtual Base::~Base() {}` — and worst, a `virtual int x;` data member,
   where the layout pre-scan (`ClassBodyDeclaresVirtual` sees the keyword
   on a `DK_SIMPLE` member) fabricated a polymorphic class: vpointer
   reserved, slotless vtable emitted, for an ill-formed program (7.1.2p1).
   All five forms now fail with typed diagnostics (7.1.2p5, 9.2).

3. **Boundary gap: explicit-qualification dispatch suppression only worked
   for implicit-`this` calls** (`sem_member.cpp`, `sem_expr.h`). The README
   Assignment Boundary lists "explicit base qualification suppressing
   virtual dispatch for supported calls", and the canonical 10.3p15 form is
   the member-access call `d.Base::f()` / `p->Base::f()` — but
   `AnalyzeMemberCall` threw `OutsideBoundary("member name form")` for any
   qualified callee (a PA15-era restriction), leaving only the
   inside-a-member `Base::f()` spelling (the `AnalyzeNamedCall` path, the
   only form the spec suite exercises). Implemented: a qualified
   member-access callee resolves its prefix against the scope model
   (`TryResolveCalleeType`), must name the object's class or a base
   (`BaseClassDistance`), looks the member up starting at the named class
   (`FindMemberBinding`), and calls directly through the existing
   inherited-member machinery (base-subobject projection included).
   Unrelated qualifiers are rejected. Verified: `d.Base::f()`,
   `p->Base::f()`, `d.Derived::f()` all lower to direct calls with correct
   projections while unqualified `p->f()` still dispatches.

4. **Stringly class-key fact** (`lower_vtable.cpp`). `ClassKeyString`
   recovered the class key for RTTI low names (`__rtti_struct_X`) by
   splitting `NamedTypeInfo::display` on a space — the exact pattern
   `type.h`'s own comment warns against ("the PA14 mangler walks these
   instead of parsing `display`"). The key exists as a typed AST fact
   (`decl.class_key`/`class_key_spelling`). Fixed: `NamedTypeInfo` gains a
   `class_key` field stamped at both class-entity creation sites; the RTTI
   renderer consumes it; the display parse is gone.

### Checked and clean (verified, no change needed)

- **Monotonicity/regressions**: every new behavior keys off
  `is_polymorphic`/`dtor_virtual`/`vslots` facts; non-polymorphic inputs
  take identical paths to PA16. Full `test-report-through-pa17` green
  before and after the audit changes (1174/1174).
- **No dummy/stub/fallback artifacts**: vtables render real slot entries
  demanded from the final-overrider identities; pure slots point at the
  declared `__cxa_pure_virtual` (shape pinned by
  `400-header-out-of-class-virtual-vtable.ref`); no interpreter, VM,
  trampoline, templated-binary, or embedded-payload substitutes anywhere in
  the slice; no test-name or source-shape acceptance gates.
- **Dispatch ownership**: sema decides direct-vs-virtual once
  (`SN_CALLEE::vtable_slot`); lowering only spells the loads. Explicit
  destructor calls stay direct — oracle-pinned
  (`400-explicit-virtual-destructor-call-nonvirtual` uses
  `this->~Derived()` and its ref names the entry directly).
- **Key-function facts**: `fn_inline_def` is stamped by
  `RecordFunctionFacts` before `OnFunctionDeclared` fires (both call
  sites), so the `defined` input to key selection is sound; out-of-class
  definitions anchor `key_defined_in_tu` by typed name+type identity, the
  same identity `MemberDefinitionKey` uses.
- **Friend declarations** never reach `RecordVirtualMember`: the friend
  path binds through `BindFriendFunction` with `current_` switched to the
  target namespace and never calls `OnFunctionDeclared`, so a friend with a
  slot-matching signature cannot corrupt slot or key facts.
- **Deleting epilogue's `operator delete` lookup**: a typed scope-model
  lookup of the implicitly-declared global deallocation function (sema
  seeds `operator new/delete[/[]]` into every unit's global scope), not a
  stringly output-side fact; operator-delete overloading is outside the
  subset. The D0 entry itself is a lowering-invented artifact with no sema
  call site, so this is the natural owner.
- **Edge rejections**: `virtual` on a static member throws;
  `virtual ~B() = 0;` (pure destructor) fails at parse; `= 0` on a
  namespace-scope function stays outside the boundary; `typedef` +
  `virtual` throws; unions with virtual members throw; vpointer
  introduction over a non-empty non-polymorphic base throws.
- **Performance**: the class-body virtual pre-scan is one shallow walk of
  direct members per class; slot matching is linear over small `vslots`
  vectors at declaration and call sites; `Write()`'s
  vtable-render/function-lower alternation is a demand-bounded fixpoint,
  not a repeated full-suite walk; `AddUnit`'s key-function registration is
  one O(classes) pass per unit; `vtables_` is a deque so references stay
  valid as rendering demands grow it. No avoidable quadratic scans, hot
  recomputation, or excessive copying (the per-derived-class `vslots` copy
  is proportional to the slot count and paid once per class).
- **File audit**: new logic in new focused files (`sem_virtual.cpp` 253,
  `lower_vtable.cpp` ~185); `sem_class.cpp` at 1452 of the 1500 cap; the
  single pre-existing `parser.h` warning is untouched since PA6; no
  hidden fragments or code moved to unchecked paths.

## Changes Made

- `dev/src/sema/sem_new.cpp` — `AnalyzeDelete`: scalar deletes of classes
  with a virtual destructor always build the destructor child and mark the
  deleting slot (`virtual_delete`), independent of `NeedsDestruction`
  (5.3.5p3). Non-virtual and array deletes are untouched.
- `dev/src/sema/decl_binder.cpp` — `BindInitDeclarator` rejects `virtual`
  and virt-specifiers on non-function declarators and on functions declared
  outside class scope (kills the fabricated-vtable `virtual int x;` case
  and block/namespace-scope `virtual`); `BindFunctionDefinition` rejects
  them on namespace-scope and out-of-class definitions (7.1.2p5, 9.2).
- `dev/src/sema/sem_class.cpp` — `BindQualifiedSpecialMember` rejects
  `virtual` on out-of-class special member definitions.
- `dev/src/sema/sem_member.cpp`, `dev/src/sema/sem_expr.h` —
  `AnalyzeMemberCall` accepts qualified member-callee names; new
  `ResolveMemberQualifier` resolves the prefix, requires same-class-or-base,
  and the call proceeds with `qualified = true` (direct, no dispatch).
- `dev/src/sema/type.h`, `dev/src/sema/decl_binder.cpp` —
  `NamedTypeInfo::class_key` recorded from `decl.class_key_spelling` at
  both class-entity creation sites.
- `dev/src/lowering/lower_vtable.cpp` — the display-parsing
  `ClassKeyString` helper is deleted; RTTI low names consume the typed
  `class_key`.
- `pa17/plan.md` — Status, Notes, `Architecture Review`, and
  `Final Architecture Review` updated to the as-built, post-audit state.

## Validation

- Scratch probes (all through the built `cppgm++ --emit-lowir -O0`):
  - qualified calls: `d.Base::f()`, `p->Base::f()`, `d.Derived::f()` emit
    direct calls with base-subobject projection; unqualified `p->f()`
    still emits the vpointer-load indirect call; `b.Unrelated::f()` exits 1.
  - virtual delete: `struct B { virtual ~B() = default; };` +
    `delete (B*)new D;` now emits the null-check, vpointer load, deleting
    slot (offset 8) load, and indirect call — previously a bare
    `operator_delete` with no destructor.
  - placement checks: namespace-scope `override` definition, out-of-class
    `override`, block-scope `virtual`, out-of-class `virtual` destructor,
    and `virtual int x;` all exit 1; in-class `virtual`/`override`/`final`
    and pure declarations still compile (pa17 suite).
- `make test-report ACTIVE_TEST_REPORT_PAS='pa17'`: 22/22.
- `make test-report-through-pa17`: 1174/1174 (all stages pa1-pa17).
- `perl scripts/cppgm_file_audit.pl --stage pa17 --paths dev/src`: pass
  (one pre-existing `parser.h` warning, unchanged since PA6).
