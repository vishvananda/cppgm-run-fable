# PA26 Plan: Non-Virtual Multi-Base Object Model, Member Pointers, dynamic_cast<void*>

PA26 extends the PA25 compiler (same `dev/cppgm++.cpp` + `dev/src/` pipeline)
with the remaining non-virtual object-model slice. No new output format: the
same LowIR family, validated by the relaxed comparator (metadata-insensitive,
top-level-order-insensitive, but instruction/temp/label text inside functions
must match the checked-in refs exactly).

## Current gaps (measured)

- `sem_class.cpp` rejects any non-empty extra base (`OutsideBoundary("multiple
  inheritance")`); `ClassInfo` has a single `base` pointer and offset-0-only
  `extra_bases` (empty classes, PA21).
- Derived-to-base adjustment is modeled as a hop *count* (`SemNode::base_hops`)
  and lowered as a single `index i8 ..., 0` regardless of depth. The pa26 refs
  emit **one `index i8 [projection=base_subobject] p, <offset>` per direct-base
  edge** on the derivation path.
- Member lookup walks a single `Scope::class_base` chain; no 10.2 ambiguity
  detection across sibling base subobjects.
- Member pointer *types* exist (`TK_MEMBER_POINTER`), and `&C::m` forms them,
  but: `.*`/`->*` are rejected in `AnalyzeBinary`; null comparison is rejected;
  base-to-derived pm conversion is absent; there is no lowering (value type,
  constants, application), no Itanium `M` type mangling, and no pm nontype
  template argument support.
- `dynamic_cast<void*>` falls through the class-only gate in `sem_cast.cpp`
  and lowers as a plain pointer copy; refs require the offset-to-top scan.

## Object model (sema owns the facts)

`ClassInfo` grows an ordered direct-base table: for each direct base, the
`ClassInfo*`/entity, its **byte offset** in the derived object, and access.
Layout rules (matching the checked-in refs):

- Bases lay out in declaration order before fields. An **empty base occupies no
  storage and sits at offset 0** (empty-base optimization; distinct empty
  subobjects may share offset 0 in this ABI - see
  100-public-qualified-base-typedef-ambiguous-subobject where both empty bases
  and their shared grandparent sit at 0).
- A non-empty base is placed at the cursor rounded up to its alignment; the
  cursor advances past its size (100-multiple-inheritance-fields: YA at 0,
  YB at 4). Class alignment accumulates the max.
- Only the **first** base may be polymorphic; the derived class inherits its
  vslots. A polymorphic (or vptr-introducing over non-empty) extra base stays
  `OutsideBoundary` (PA27 territory: polymorphic MI / virtual bases).

`cls->base`/`base_entity` keep pointing at the first direct base so the
polymorphic chain, vtable inheritance, and existing single-base fast paths are
unchanged for PA25-subset programs (monotonic-extension rule).

## Derivation paths replace hop counts

A central helper computes the derivation path from a derived class to a base
class as a **list of per-edge byte offsets** (each edge = one direct-base
step), and detects **ambiguity** (more than one subobject path, 10.2 subset)
and unrelatedness. `SemNode` carries the path (`base_steps`) instead of a bare
count; lowering emits one `index i8 [projection=base_subobject]` per edge with
that edge's offset. All producers switch to paths:

- member access / implicit `this` adjustment (`sem_member.cpp`)
- conversions and casts to base pointers/references (`sem_apply.cpp`,
  `sem_cast.cpp`)
- synthesized special members (`ThisBaseAddress` et al.)
- const-eval address paths keep working: non-zero base offsets participate in
  address computation where required.

Overload ranking keeps using the path length as `base_distance`.

## Member lookup across multiple bases

`Scope` grows sibling base links (all direct bases' member scopes). Unqualified
and member lookup walk the base DAG depth-first; a name found in more than one
sibling subobject (different entities) is an ambiguity error
(100-bad-ambiguous-member must keep failing for the *right* reason; the
SFINAE test 300-ambiguous-member-nontype-arg-sfinae needs the ambiguity to be
an ordinary substitution failure in immediate context). Names found in an
instantiated base's scope that are template-parameter bindings do not act as
inherited member typedefs (100-pack-expanded-base-template-parameter-lookup).

## Generated special members across bases

Synthesized default ctor, copy ctor, copy assignment, and dtor loop over the
direct-base table in order (reverse order for destruction), forming each base
subobject address from the base's offset (C2 `__base_entry` variants, as
today). Explicit mem-initializers pick the *named* base out of the table;
remaining bases default-construct in declaration order. Pack-expanded
mem-initializers (`store<I, T>(u)...`) expand alongside pack-expanded base
specifiers. Triviality/`copyobj` classification is per-class and already
whole-object; multi-base trivially-copyable classes keep lowering to `copyobj`
(100-multiple-inheritance-copy).

## Member pointers

Representation (fixed by the refs, Itanium-flavored with 0 as null):

- **Data member pointer** = `i64` holding `field-offset + 1`; null = `0`.
  Application: `sub i64 v, 1` then `index i8 [projection=field] obj, off`.
- **Function member pointer** = `i128`; low 64 bits = target function address,
  high 64 bits = this-adjustment; null = `0`. Formation: `addr @fn` -> `copy
  i64` -> `convert zext i128 i64`. Application: `convert trunc i64 i128` ->
  `copy ptr` -> indirect `call ... as (sig)`; the refs apply no dynamic
  adjustment at call sites (all checked-in cases have adjustment 0), so call
  sites match that shape; conversions still fold the static base offset into
  the value so data pms through non-zero bases stay correct.

Sema work:

- `.*` / `->*` in `AnalyzeBinary`: object operand class must be the pm class or
  derived from it (derived-to-base path applies to the object address); result
  is a field lvalue (data pm) or a bound-member callable (function pm) usable
  only as a call target.
- Null pm handling: `0`/`nullptr` conversion already exists; comparisons
  against null lower as `cmp eq/ne i64|i128 v, 0`.
- Base-to-derived pm conversion (4.11p2) and pm qualification conversions
  (adding const/volatile to the member type).
- `&C::f` over an overload set resolves against the pm target type
  (existing function-pointer target machinery extended to pm targets).
- pm nontype template parameters: template argument conversion, constant
  evaluation of `&C::m` to a typed pm constant (offset / function entity),
  argument identity for specialization dedup, deduction (`template<class U,
  U>` with `&C::size`), and SFINAE on ambiguous/missing members.
- Mangling: `M<class><member-type>` type manglings and `Xad L_Z...E E`
  nontype pm arguments (the comparator pairs functions by their `object=`
  Itanium names, so these must match g++).

Lowering: `LowerValueType` maps data pm -> `i64`, function pm -> `i128`;
constants and `.*`/`->*` application per the shapes above; pm-typed fields,
parameters, and globals fall out of the value-type mapping.

## dynamic_cast<void*>

Sema: accept `dynamic_cast<void*>(p)` for pointer-to-polymorphic-class
operands (prvalue result), producing `SN_DYNAMIC_CAST` with a void* target.
Lowering (matches 100-dynamic-cast-void.ref exactly):

- slot `$dyn_cast__N : ptr`; `store ptr 0`; `cmp eq ptr v, 0`;
  `branch -> ^dyn_cast_end / ^dyn_cast_scan`.
- `^dyn_cast_scan`: load vptr; `index i8 vptr, -16` (offset-to-top slot);
  `load i64`; `index i8 v, off`; store; jump end.
- A **dead runtime block** (same presentation idiom the reference uses for the
  reference-form dynamic_cast): `addr @__rtti_class_Src`; `addr
  @__external_rtti__void` (`_ZTIv`); `call ptr
  @__external_runtime____dynamic_cast(v, src, dst, -2)`; store; jump end.
- `^dyn_cast_end`: load the slot.

This reuses `RttiRef`/`ExternalRuntimeFnRef`; the only new external is the
`_ZTIv` void type_info global.

## Ownership boundaries

- `sema/class_info.*` owns base tables, offsets, and path/ambiguity queries.
- `sema/` owns all semantic facts as typed state (paths on nodes, pm constants
  in const-eval values); lowering only renders them - no re-derivation from
  text.
- `lowering/lower_member.cpp` owns base-step emission; `lower_eh.cpp` owns the
  dynamic_cast shapes; `lower_types.cpp`/`lower_expr.cpp` own pm value forms.
- `paN/` handouts, refs, and harness scripts are not touched.

## Validation

1. Fast loop: `make test-report ACTIVE_TEST_REPORT_PAS='pa26'` and
   `make -C pa26 check TEST=...` per fixture.
2. After each sema/lowering slice: root `make test-report-through-pa26` -
   older-stage regressions (especially PA15-PA25 base-adjustment shapes, which
   move from "one hop" to per-edge emission) are blockers, fixed before more
   PA26 feature work.
3. `perl scripts/cppgm_file_audit.pl --stage pa26 --paths dev/src` for file
   size/architecture gates.
4. Manual spot-check: feed generated LowIR for the new shapes through PA28
   `lowir2native` where execution semantics matter (multi-base field layout,
   member pointer calls).

## Sequencing

1. Multi-base core: ClassInfo base table + layout + paths + lookup/ambiguity +
   special members + per-edge lowering (unlocks 100-multiple-inheritance-*,
   the qualified-typedef shape test, inherited-explicit-bool, ambiguity tests).
2. Pack-expanded non-empty bases + pack-expanded mem-initializers.
3. Member pointers: sema ops + conversions + lowering + mangling + nontype
   args/deduction/SFINAE (unlocks the ~27 pm tests incl. spec).
4. dynamic_cast<void*> shape.
5. Sweep the remaining misc tests; through-check; audit; commit in cohesive
   slices per phase.

## Outcome Notes

- An adjustment lowers as **one** base-subobject projection carrying the
  unique derivation path's *total* byte offset (preserving the PA15-25
  single-projection shapes); a qualified call chains two projections
  (naming class, then member owner), which is what the
  qualified-typedef fixture pins.
- The reference presentation quirks the fixtures pin, reproduced as
  general rules: an instantiated const function-pointer static member's
  read folds to the address of a symbolic declare-only function named
  by the member (storage keeps its dynamic init); `&&`/`||` pointer
  right-operands test in the `ptr` value space while member pointers
  test in `i64`.
- Partial-explicit function template-ids stay overload sets when a
  sibling template's remaining parameters could deduce from the use
  context (14.8.1); associated-namespace collection carries a visited
  set (CRTP bases no longer recurse).
- New files: `sema/sem_bases.cpp` (base-clause binding),
  `sema/sem_member_pointer.cpp` (pm semantics + nontype args),
  `lowering/lower_member_pointer.cpp` (pm value/access/call lowering).
