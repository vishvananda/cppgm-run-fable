# PA27 Audit

## Audit Plan

Scope: commits `3ca26d4e9..e8a63bf25` (PA27 slice: virtual-base layout
and access, hidden vbase-pointer ABI, secondary vtable views,
construction vtables + VTT, sibling `dynamic_cast`, basecast shapes,
anonymous-namespace manglings), audited against `pa27/README.md`,
`pa27/plan.md`, the 26 local refs, and the pa14-pa26 regression
surface.

### Files to inspect

- `dev/src/sema/class_info.{h,cpp}` (+449/+180): the vbase table
  (`ComputeVirtualBaseTable`, `PostOrderVBases`), nv/full layout split
  (`BeginClassLayout`/`FinishClassLayout`), primary-base promotion,
  view collection (`ComputeClassViews`), view final-overrider
  resolution (`ResolveViewOverrider`), and the virtual-path queries
  (`VirtualBasePath`, `CompleteObjectOffset`, `FirstVirtualEdge`).
  Verify all facts are typed (class records + indices, no strings),
  walks are DAG-bounded, and layout rules are general rather than
  fixture constants.
- `dev/src/lowering/lower_vbase.cpp` (new, 981): hidden-param
  computation (`HiddenSignatureParams`, `HiddenParamsForType`),
  template demand analysis (`CollectVBaseDemands`), per-parameter
  carried maps (`EmitParamVBasePointers`), call-site supply
  (`SupplyVBaseEntry`, `AppendHiddenArguments`), virtual-edge member
  paths (`VBaseSubobjectAddress`, `VBaseRemainderHops`), VTT slicing,
  vpointer installs, comdat pairing, alias suppression. Verify the
  parameter-identity tracking (name-keyed `vbase_params_`) is
  scope-safe, table indices are remapped between class contexts, and
  the demand analysis cannot silently change non-template signatures.
- `dev/src/lowering/lower_vtable.cpp` (+513): primary/view/construction
  vtable rendering, VTT shape and indices, vmi RTTI records, thunks.
  Verify the header-writer table order agrees with every header-reader
  index (`-24-8k` loads, RTTI virtual-base offset-flags rows), and that
  the decoded dialect rules (view headers carry the complete class's
  table, ref-pinned by the V view of the 200 test) stay internally
  consistent on shapes the refs do not cover.
- `dev/src/lowering/lower_eh.cpp` (+25): the Itanium `src2dst` hint
  (`DynamicCastHint`). Verify hint values match the ABI contract
  (public unique path required for a static offset) since PA28+ runs
  the output natively against the real `__dynamic_cast`.
- `dev/src/sema/sem_bases.cpp`, `sem_virtual.cpp`, `sem_ctor.cpp`,
  `sem_special.cpp`, `sem_member.cpp`, `sem_cast.cpp`,
  `sem_convert.cpp`: virtual base acceptance, primary selection, view
  slot recording, vbase construction/destruction phases and transfer
  synthesis, virtual-edge member paths, upcast reference views,
  pointer conversions. Verify order rules (12.6.2p10 / 12.4p7) are
  implemented from the typed tables and `vbase_action` gating is
  entry-kind-driven, not test-driven.
- `dev/src/lowering/lower_convert.cpp`, `lower_member.cpp`,
  `lower_expr.cpp`, `lower_function.cpp`, `lower_unit.cpp`,
  `lower_name.cpp`: basecast shapes, `known_nonnull` guard elision,
  dispatched-call EH wrapping, hidden-argument rows, alias sections,
  `_GLOBAL__N_1` manglings. Verify no fallback success paths and no
  output shaped by test names.
- Commit `e8a63bf25` ("satisfy the file audit in substance"): verify it
  moved real code into `lower_vbase.cpp` rather than hiding
  implementation in unchecked paths, and that comment trimming did not
  remove load-bearing contracts.

### Performance risks

- `CountBasePaths` memoization (linear per query) on the hot member
  path; `ViewPathSearch`/`ResolveViewOverrider` per slot per view;
  `RenderPendingVTables` rescans; `HiddenSignatureParams` caching
  (`hidden_ready`); `CollectVBaseDemands` full-definition walks per
  instantiated function; `VttShapeSize` recursion per index query.
  Confirm each is bounded by class-graph size and runs per definition,
  not per expression.

### Ownership boundaries

- sema owns vbase tables, layout, views, overrider resolution, and
  ctor/dtor plans (class_info + sem_*); lowering owns hidden-param
  signatures, supply, and rendering. Verify lowering never re-derives
  sema facts from strings and sema never emits LowIR text.

### File-audit issues

- `perl scripts/cppgm_file_audit.pl --stage pa27 --paths dev/src`
  passes with 6 pre-existing bad-division warnings (headers with
  implementation bodies, all predating PA27). Confirm no new
  implementation moved to headers to dodge the size gates and file
  sizes stay under limits after fixes.

## Findings

1. **Wrong vbase-table index against a parameter's carried pointers
   (miscompile, in-boundary).** `FunctionLowerer::VBaseSubobjectAddress`
   (`dev/src/lowering/lower_vbase.cpp`) received `vbase_index` in the
   table of the *object expression's static class*, but indexed the
   parameter's `__pvb` slots / carried registers, which are laid out in
   the *parameter's declared class's* table. The two tables can order
   entries differently (e.g. `f(IOStream& x)` accessing a member
   through `static_cast<BasicIOS&>(x)`: `index_BasicIOS(IOS) = 0` but
   `index_IOStream(IOS) = 1`), silently supplying the wrong subobject
   address. `SupplyVBaseEntry` already remapped by class
   (`FindClassVBase(*map.cls, ...)`); the member path did not. Fixed by
   remapping the carrier class through the parameter's own table before
   touching slots/carried entries.

2. **Name-only parameter identity lets locals shadow the hidden-pointer
   maps (ICE or miscompile).** The `vbase_params_` lookups keyed on the
   bottom id-expression's *name* alone. A block-scoped local shadowing
   a same-named vbase-class parameter matched the parameter's map: the
   reference-parameter form then threw "unlowered local" from the
   scope-keyed `SlotRef` (ICE on legal code), and the collapsed-pointer
   form silently used the parameter's hidden register for the local's
   access. The template demand analysis (`BottomParamIndex`,
   `MentionsParam`) had the same name-only matching, so a shadowing
   local could add phantom demands to an instantiated signature. Fixed
   by recording the declaring scope in `VBaseParamMap` and matching
   `(name, scope)` in all three places.

3. **Header-reader/writer index disagreement on shapes the refs never
   pin (latent self-miscompile).** The vtable-header readers (the
   `vptr - 24 - 8k` loads in `VBaseSubobjectAddress` /
   `SupplyVBaseEntry`, and the RTTI vmi `(-24-8k)<<8|3` offset-flags
   rows) use the *static* class's vbase-table index, while every header
   in a class's vtable group is written in the *complete* class's table
   order (ref-pinned: the V view in the 200 test carries D's one-row
   header even though V itself has no vbases). All 26 refs only cover
   hierarchies where the indices agree, so the decode is consistent
   there — but a polymorphic hierarchy where a subobject class's table
   order differs from the complete class's (e.g. `struct C : A, Mid`
   where A's virtual base enters C's DFS table ahead of Mid's) would
   read the wrong header slot at runtime. The refs do not define the
   ABI for such shapes; rather than silently miscompiling, class
   completion now rejects a polymorphic class whose vbase-table indices
   disagree with any vbase-carrying subobject class's own table, using
   the established assignment-boundary error convention.

4. **`dynamic_cast` src2dst hint ignored access (wrong runtime
   behavior from PA28 on).** `DynamicCastHint`
   (`dev/src/lowering/lower_eh.cpp`) returned the static offset for any
   unique non-virtual path. The Itanium contract requires a *public*
   unique path for a non-negative hint; a private or protected base
   must yield -2, otherwise the runtime's shortcut succeeds casts that
   must fail. Fixed by walking the unique path's edges
   (`BaseAccessPath`) and demanding public access on every edge.

Reviewed and found sound (no action): the layout split
(nv/full, primary promotion, empty-base rules) is computed from typed
tables; ctor/dtor vbase phases are entry-kind-gated (`vbase_action` +
C2/D2/D0), matching 12.6.2p10/12.4p7; the demand analysis only filters
*template* signatures (non-template signatures always carry the full
set); the file-audit commit moved real machinery into
`lower_vbase.cpp` with no behavior change and trimmed only redundant
comments; no fixture names, no environment gates, no fallback success
paths, no interpreter/VM/embedded-payload substitutes anywhere in the
slice; hot-path helpers are memoized (`CountBasePaths`,
`HiddenSignatureParams`) or bounded by class-graph size.

## Changes Made

- `dev/src/lowering/lower_vbase.cpp`: `VBaseSubobjectAddress` remaps
  the carrier virtual base into the parameter's own table
  (`FindClassVBase`) before indexing `__pvb` slots or carried
  registers, and falls through to the generic path when the carrier is
  not in the parameter's table; `VBaseParamMap` lookups (member path
  and call-argument supply) now require the id-expression's declaring
  scope to match the parameter's; the template demand analysis matches
  parameters by `(name, scope)`.
- `dev/src/lowering/lower_function.h`: `VBaseParamMap` records the
  parameter's declaring scope.
- `dev/src/sema/sem_virtual.cpp`: `FinishClassVirtualFacts` rejects a
  polymorphic class whose vbase-table indices disagree with a
  vbase-carrying subobject class's own table (the vtable-header /
  RTTI-row reader contract), with an assignment-boundary error.
- `dev/src/lowering/lower_eh.cpp`: `DynamicCastHint` returns -2 unless
  the unique non-virtual path is public on every edge.

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa27 --paths dev/src`:
  pass (6 pre-existing bad-division warnings, unchanged).
- `make test-report-through-pa27`: 2507/2507 tests passing after the
  fixes (pa27 local: 26/26), matching the pre-audit count - no
  regression, no ref divergence from the corrected supply paths.
- Negative checks (hand-built reproducers, compiled before and after):
  the `IOStream`/`BasicIOS` base-view access lowered to the wrong
  `__pvb` slot before (loaded `$x__pvb0` = BasicIOS for a member of
  IOS = entry 1) and now loads `$x__pvb1`; the shadowing-local case
  ICEd with "unlowered local x__pvb0" and now lowers the local's own
  storage with a static projection; the disagreeing-table polymorphic
  hierarchy compiled silently before (wrong header slot at runtime)
  and is now rejected with the boundary error; the private-base
  `dynamic_cast` hint was `0` and is now `-2`.
