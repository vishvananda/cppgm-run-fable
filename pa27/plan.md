# PA27 Plan: Multi-Vtable / Virtual-Base ABI Slice

PA27 extends the PA26 compiler with (1) virtual inheritance layout and
access, (2) polymorphic multiple inheritance with secondary vtable
views, (3) pointer-form sibling `dynamic_cast`, and (4) RTTI through
non-primary views. The checked-in `.ref` files are the contract; the
rules below are decoded from all 26 refs.

## Object layout (sema, class_info)

- Non-virtual (nv) region first, then virtual-base (vbase) subobjects,
  complete-object only. New `ClassInfo` facts: `vbases` (table),
  `nv_dsize`/`nv_size`/`nv_alignment`, plus the complete `dsize`/`size`.
- Primary-base promotion: if the class is polymorphic, the FIRST
  polymorphic non-virtual direct base moves to offset 0 and carries the
  primary vtable chain (`derived : tag, mid` lays out mid@0, tag@8).
  Other nv bases follow in declaration order. Construction/destruction
  order stays declaration order.
- Virtual direct bases do not join the nv region; they are recorded in
  the vbase table. Vbase table order = first appearance on the
  depth-first left-to-right walk of the inheritance DAG, deduplicated
  (e.g. `E : virtual H`, `H : virtual B` -> [H, B]).
- A class with any vbase is never empty. As an nv base subobject it
  spans `round_up(max(nv_dsize,1), alignment)` where `alignment` is the
  full alignment (nv + vbases). Example: `YB : virtual YA {}` spans 4.
- Complete layout appends each vbase at `round_up(cursor, nv_align(vb))`
  spanning `nv_dsize(vb)` (min 1): `E : virtual H` = E-nv@0(1), H@1(1),
  B@4(4), sizeof 8. `sizeof` rounds by full alignment.
- 10.2 member lookup includes virtual bases (scope extra-base links);
  shared vbase subtrees are deduped by the existing visited-set walk.

## Vtable model (per class C)

- Primary slot list: unchanged inheritance along the (promoted) primary
  chain. Overrides of non-primary-base virtuals do NOT append primary
  slots; new virtuals do. The implicit/declared dtor overrides dtor
  slots in every polymorphic base view.
- Primary vtable global `@C__vtable` [_ZTVC]:
  - no vbases: `{i64 0 (otop), rtti, slot...}`, address point +16 (PA26).
  - with vbases: `{i64 vbase_off..., i64 0, rtti, [fptr, i64 adj]...}`,
    address point +24 (one vbase): slots become [fptr, this-adjust]
    pairs; the header carries C's vbase offsets (entry k at ap-24-8k).
- Secondary "views": one global `@C____view__<B>__<off>__vtable`
  (object= its own @name) for every polymorphic base subobject (nv or
  virtual, found transitively) not on the offset-0 primary chain.
  - If C has no vbases: headerless bare slot array (ap +0); slots are
    the base's slot list resolved to C-level final overriders, emitted
    as direct fn addr when no this-adjust is needed, else a thunk
    `_<lowname>__vtable_return_adjust` [object=_ZThn<off>_<mangled>]
    whose body does `index i8 %arg0, -off; tail-call overrider`.
  - If C has vbases: header `{i64 C-vbase-offsets rel. to view, i64
    -off (otop), rtti C}`, ap +24; slot shape follows the view base's
    own shape: paired [fptr, adj] (adj = overrider_off - view_off, no
    thunks) when the base itself has vbases, unpaired thunk slots when
    the base is vbase-less (e.g. the V view in the 200 test).
- Construction vtables + VTT for classes that are polymorphic AND have
  vbases: `@C____vtt` [_ZTTC, object_root=yes] = [C primary ap] ++
  (for each direct nv base X that is polymorphic with vbases: sub-VTT
  entries `@C____construction__<X>__<xoff>__s<K>__vtable` shaped like
  X's own VTT, recursively) ++ [C's non-primary view aps]. Construction
  vtable content: X-level final overriders; header offsets computed at
  X's position inside C; rtti = X for s0, the view base for sK.
- RTTI: no bases -> __class_type_info; exactly one public nv direct
  base at offset 0 -> __si_class_type_info; otherwise
  __vmi_class_type_info with `i32 0, i32 nbases` then per direct base
  (declaration order) `ptr base-rtti, i64 offset_flags` where
  offset_flags = (offset<<8)|2 for nv public bases and
  ((-24 - 8*table_index)<<8)|3 for virtual bases (vtable header slot).

## Constructor/destructor protocol

Naming keeps the PA26 conventions (@C__C, @C__C__base_entry, __ovN,
D1/D0/D2 object names, `alias object C2 = C1-fn` when no separate C2
body is demanded).

- Non-poly class with vbases:
  - C2 `(this, __vbptr0..N-1)`: hidden ptr per vbase-table entry; body
    never constructs vbases.
  - C1: construct vbases in table order (base_entry at static offset,
    passing the callee's own vbase table filtered from C's), then nv
    bases (C2s with vbptrs = C's vbase addrs), then fields/body.
- Poly class with vbases: full Itanium-style VTT protocol:
  - C2/D2 `(this, __vtt, __vbptr0..)`: nv-base C2/D2 calls pass
    `__vtt + 8*subvtt_index` and forward __vbptrs; vptr installs load
    from the VTT (own primary = vtt[0], each view location from its
    tail entry), stores at static own-layout offsets.
  - C1: construct vbases, nv-base C2s with `@C____vtt + 8*idx` and
    static vbase addrs, then install static vptrs (primary + views).
  - D1: install static vptrs, body, nv-base D2s (reverse order), then
    vbase D2s (reverse table order). D0: same as D1 but WITHOUT the
    vbase destructor calls, then operator_delete (matches refs).
  - Ctor/dtor bodies of poly classes read nothing from __vbptrs; the
    hidden args are still declared and forwarded.
- Implicit copy/move ctors: the `other` reference param takes NO hidden
  pvbs; `$other__pvbN` slots are reconstructed from `other + static
  complete-layout offset`, in post-order (children-before-parent) DFS
  order of the vbase graph ([B, H] for E; [A, B, C] for flat D).

## Hidden argument ABI for ordinary functions

- Reference-to-C parameter (including reference-to-pointer-to-C), C has
  vbases, non-template function (incl. user ctors/methods, and bare
  declarations): trailing hidden `%__pvbptrK : ptr`, one per entry of
  C's FULL vbase table, appended after all declared params in param
  order, numbered sequentially across the signature.
- Pointer-to-C parameter: table entries that are vbases of C's direct
  virtual bases collapse into the direct vbase (read(OStream*) carries
  only BasicIOS; IOS is found at its static offset inside BasicIOS).
- Template specializations: pvb set is demand-driven from the
  instantiated body: a member access hopping into a vbase demands that
  carried entry; passing the reference as an argument to another
  function/ctor demands the FULL table. No demand -> no hidden param;
  non-carried entries reconstruct from static complete-layout fallback.
- Member functions of a NON-polymorphic class with vbases take
  `%__vbptrN` (own table) appended after declared params; polymorphic
  classes' methods take none (they use dynamic vptr offsets).
- Prologue: reference params of vbase classes get `$<name>__pvbN : ptr`
  slots right after the param's own slot; hidden args store first,
  fallback reconstructions compute `index i8 %param, off` from the raw
  param register.
- Call sites supply pvbs:
  - complete-object argument (known dynamic type): static offsets.
  - poly static class: `load vptr; load i64 at vptr-24-8k; index`.
  - non-poly reference/pointer: static complete-layout fallback.
  - forwarding from a function that carries pvb slots: load the slot.
- Returns carry nothing; callers use static fallback on results.
- Member access through a vbase: static class poly -> dynamic vptr
  offset load (even for locals); else static complete-object offset.

## Casts and RTTI queries

- Null-checked displaced adjustments (pointer up/down casts) change
  from the PA26 `ptradj` shape to the ref `basecast` shape:
  `$basecast__N` slot; blocks ^basecast_null_i (store 0),
  ^basecast_adjust_i+1 (projected index), ^basecast_end_i+2 (load).
  No ref outside pa27 pins the old shape.
- `this`-sourced downcasts skip the null check (direct projected
  index); reference casts stay direct (existing SN_MEMBER_EXPRESSION
  base_reverse path).
- Sibling pointer dynamic_cast: sema accepts polymorphic-source casts
  where the target is an unrelated class; lowering keeps the existing
  dyn_cast shape with hint = -2 when the target does not derive from
  the source (existing downcasts keep 0).
- typeid: existing dynamic form (vptr-8 rtti load) already matches the
  refs through non-primary views once the classes are accepted.

## Ownership boundaries

- sema owns: vbase tables, nv/full layout split, primary promotion,
  slot lists + per-view final-overrider resolution, ctor/dtor plans.
- lowering owns: hidden-param signatures, pvb slot management and call
  supply, vtable/view/VTT/construction-vtable rendering, thunks,
  basecast shapes, RTTI records.
- New sema facts live in class_info.{h,cpp} + sem_bases.cpp +
  sem_virtual.cpp; new lowering surfaces split into focused files
  (lower_vbase.cpp for the hidden-arg ABI, lower_vtable.cpp grows the
  view/VTT rendering) to satisfy the file audit.

## Validation

- `make test-report ACTIVE_TEST_REPORT_PAS='pa27'` for the 26 local
  tests while iterating; each ABI slice is pinned by named tests.
- Full `make test-report-through-pa27` after each subsystem lands:
  PA17-26 vtable/ctor/dtor refs guard the "monotonic extension" rule
  (classes without vbases / single-poly-chain classes must lower
  byte-identically to PA26).
- `perl scripts/cppgm_file_audit.pl --stage pa27 --paths dev/src` for
  size/architecture gates before committing.

## Architecture Review

The implementation matches the ownership split above. Sema facts are
typed end to end: `ClassInfo` gains the vbase table (`vbases`), the
nv/full layout split (`nv_dsize`/`nv_size`/`nv_alignment`), primary
promotion (`primary_base`), per-class views (`views`), and
`declared_virtuals` for view final-overrider resolution
(`ResolveViewOverrider`). Virtual-edge member paths ride SemNodes as
`vbase_index` + `base_offset` (carrier table entry plus static
remainder), and the complete-object-only construction/destruction
phases are marked with `vbase_action` and gated by entry kind
(C1/D1 emit, C2/D2/D0 drop but keep the callee demand) - no strings,
no test-shaped gates.

Lowering owns the hidden-argument ABI in `lower_vbase.cpp`: signature
computation (`HiddenSignatureParams`, cached per registry entry),
template demand analysis over instantiated bodies, per-parameter
carried maps and `$name__pvbN` slots, call-site supply
(`SupplyVBaseEntry` with anchor sharing), VTT slicing, and vpointer
installs from static groups or the construction table. Vtable
rendering (`lower_vtable.cpp`) derives views, construction vtables,
VTT shapes, and vmi RTTI records from the sema tables; thunks are
generated per (target, adjust) pair. `basecast` shapes and
`known_nonnull` guard elision live in `lower_convert.cpp` beside the
older adjustment forms.

Audit-driven corrections (see `pa27/audit.md`): the virtual-edge
member path now remaps the carrier into the parameter's own vbase
table before touching carried pointers (the node's index counts the
object's static class's table, which can order entries differently
across a base view); parameter-map lookups match the declaring scope
so shadowing locals never ride a parameter's hidden pointers; the
`dynamic_cast` src2dst hint demands public access on every edge of
the unique path; and class completion rejects polymorphic hierarchies
whose subobject vbase-table orders disagree - the decoded dialect
writes every header of a class's vtable group in the complete class's
table order while readers index it with their static class's table,
and the refs only define the agreeing shapes.

## Final Architecture Review

Regression surface: PA14-PA26 outputs are byte-stable - classes
without virtual bases take the `vbases.empty()` fast paths everywhere
(no hidden params, headerless PA26 vtables, PA26 address points), and
the full through-pa27 suite passes. The primary-promotion and
empty-base rules generalize the PA26 layout instead of forking it.

Performance: all new queries are bounded by class-graph size and
memoized where hot - `CountBasePaths` memoizes per query,
`HiddenSignatureParams` caches per function entry (`hidden_ready`),
demand analysis walks each instantiated definition once, and vtable
groups render once per demand level (`rendered`/`group_rendered`).
No quadratic full-registry scans were added; `RenderPendingVTables`
rescans only on new demand rounds, matching the PA17 fixpoint.

Boundaries: no compiler phase is skipped, there are no fallback
success paths, no interpreter/VM/trampoline or embedded-payload
substitutes, and no fixture-specific acceptance. The one deliberate
boundary narrowing (disagreeing vbase-table orders under a
polymorphic class) is a typed completion-time rejection of shapes
whose ABI the reference outputs leave undefined, following the
established assignment-boundary convention rather than guessing an
encoding the harness could never check.
