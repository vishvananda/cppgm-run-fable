# PA17 Plan: Virtual Functions, VTables, Dynamic Dispatch

## Goal

Extend the PA16 compiler with the scoped single-inheritance polymorphism layer:
virtual member functions and destructors, override/final checking, pure virtual
declarations, vtable + RTTI-lite global emission, vpointer stores in
constructors/destructors, and indirect LowIR calls for virtual dispatch.
Oracle: checked-in `.ref` LowIR under `pa17/tests/` (relaxed compare keeps
global names, canonicalizes function names, ignores `object=`/`binding=`
metadata and `alias object` lines, and enforces vtable slot order).

## Reference contract (from refs + lowir.md)

- Vtable global per emitted polymorphic class `X`:
  `global @X__vtable [storage=readonly, binding=weak|strong, object=_ZTV<mangled>] =
  { i64 0, ptr addr @__rtti_<key>_<X>, <slot>... }` where `<key>` is the
  class-key (`class`/`struct`). Slots in virtual-slot order: inherited slots
  first (an override keeps its slot), new slots in declaration order; a virtual
  destructor takes two adjacent slots: complete (D1) then deleting (D0). Pure
  slots point at `@__cxa_pure_virtual`.
- RTTI: `@__rtti_<key>_<X>` = `{ ptr addr @__external_rtti_vtable____class_type_info + 16,
  ptr addr @__typeinfo_name__<key>_<X> }` for a root class;
  `__si_class_type_info` variant plus a third `ptr addr @__rtti_<base>` item for
  a derived class. `@__typeinfo_name__<key>_<X>` holds the `<len><name>` bytes
  plus NUL. Emitted (with the base chain, transitively) whenever `X`'s vtable is
  defined; external abi declares (`declare global @__external_rtti_vtable____*`)
  on demand.
- Vpointer store in ctors (after base construction, before member inits) and at
  destructor entry (before body/member/base destruction):
  `%a = addr @X__vtable; %b = index i8 %a, 16; store ptr %b, <this>`.
- Virtual call: `%v = load ptr <obj>; [%s = index i8 %v, 8k;] %f = load ptr %v|%s;
  call <ret> %f(<obj>, args) as (<lowered signature>) -> <ret>`.
- `delete p` with virtual dtor dispatches through the deleting slot inside the
  existing null-check shape (replaces direct dtor + operator_delete calls).
- Deleting entry function `X___X__deleting_entry` (`object=...D0Ev`): D1 body +
  trailing `call void @operator_delete(<this>)`.
- Entry families for polymorphic classes: demanded base entries (C2/D2) are
  separate functions; when any entry of a *user-provided* ctor/dtor of a
  polymorphic class is emitted, the complete entry (C1/D1) is emitted too;
  every emitted complete entry appends `alias object <C2/D2> = @<complete>`
  (existing PA16 behavior). Non-polymorphic classes keep PA16 behavior.
- Vtable emission triggers: (a) demand — any reference from emitted code
  (vpointer stores) emits it weak when the class has no key function, or a
  `declare global` when the key function is defined in another TU; (b) key
  function (first declared non-pure virtual member, incl. the destructor, not
  defined in-class) defined out-of-class in this TU emits it strong.

## Ownership boundaries

- **sema/class_info.h** — typed vtable metadata on `ClassInfo`:
  `is_polymorphic`, `declares_virtual` (own `virtual` keyword pre-scan),
  `base_offset`, `dtor_virtual`, `dtor_slot`, `vslots`
  (`VirtualSlot { name, type, owner scope, pure, final, is_dtor_complete/
  deleting }`), key-function identity (`key_name`, `key_type`,
  `key_defined_in_tu`). Layout reserves the vpointer at offset 0 when the class
  introduces it; triviality/effect queries treat polymorphic classes as
  non-trivial (copy/move/assign), needing construction, and having
  construction/destruction effects; dtor triviality also keys off
  `dtor_virtual`.
- **sema/sem_virtual.cpp** (new) — slot building at declaration time
  (`RecordVirtualMember`, `RecordVirtualDtor`), override matching (10.3p2:
  name + parameter list + cv), covariant return checking (10.3p7 subset),
  `override`/`final` diagnostics, key-function computation, call-site
  classification (`ResolveVirtualSlot`) used by the expression analyzer, and
  the vpointer-store node factory.
- **sema** integration: `type_builder` records `override`/`final` instead of
  throwing; `decl_binder`/`sem_binder` pass specs + declarator + pure flag
  through `OnFunctionDeclared`; `= 0` on a member function declarator is the
  pure-specifier; `sem_class` accepts `virtual` destructors;
  `sem_ctor`/`sem_special`/`sem_class` insert `SN_VPOINTER_STORE` actions into
  user and synthesized ctor/dtor bodies; `sem_member` marks unqualified calls
  to virtual members with the slot index (qualified callee names and explicit
  destructor calls suppress dispatch); `sem_new` marks virtual-dtor `delete`
  for deleting-slot dispatch.
- **lowering/lower_vtable.cpp** (new) — vtable/RTTI/typeinfo registry and
  rendering on `LowerProgram`: `VTableRef(class)` (demand + low name),
  deterministic item rendering (demands slot functions, deleting entries, RTTI
  chain, abi declares, `__cxa_pure_virtual`), strong/weak/declare-only
  resolution, and the Write()-side fixpoint with function lowering.
- **lowering** integration: `lower_function`/`lower_member` lower
  `SN_VPOINTER_STORE`; `lower_expr::LowerCall` emits the indirect dispatch for
  slot-marked callees; `lower_new::LowerDelete` dispatches through the deleting
  slot; `lower_unit` emits deleting entries (`special_code == "D0"`), co-demands
  complete entries for user-provided polymorphic ctors/dtors, and hooks
  key-function registration; `lower_name` gains `_ZTV`/`_ZTI`/`_ZTS` manglers;
  base-subobject projections honor `base_offset`.

## Validation plan

- `make test-report ACTIVE_TEST_REPORT_PAS='pa17'` for the pa17 suite while
  iterating; `make -C pa17 check TEST=...` for single cases.
- `make test-report-through-pa17` as the required exit gate (monotonic: PA16
  and earlier outputs must not change for non-polymorphic inputs — vtables,
  vpointer stores, and dispatch are driven only by polymorphic class facts).
- `perl scripts/cppgm_file_audit.pl --stage pa17 --paths dev/src` (new code in
  new focused files; sem_class.cpp is near the 1500-line cap).

## Status

- [x] Plan
- [x] Sema: virt-specifier/pure parsing, slot building, override/final checks
- [x] Sema: vpointer actions, dispatch classification, layout/triviality facts
- [x] Lowering: vtable/RTTI emission, dispatch, deleting entries, delete path
- [x] pa17 suite green (22/22); through-pa17 green (1174/1174); file audit clean

## Notes from implementation

- The complete ctor/dtor entry co-emits with a demanded base entry only for
  user-provided members of classes with a *virtual destructor* (the refs pin
  this: `400-inline-polymorphic-constructor-vtable` emits `Base__Base`,
  `400-header-out-of-class-virtual-vtable` does not emit the base's C1).
- A class with a virtual destructor synthesizes its implicit destructor at
  class completion so the vtable's D1/D0 slots always have demandable
  definitions (`400-key-function-vtable-without-local-construction`).
- Local base-reference bindings (`Base& b = d;`) project to the base
  subobject like reference arguments already did.
- Introducing a vpointer over a non-empty non-polymorphic base would need
  base-pointer adjustment and stays outside the boundary (rejected).
