# PA14 Plan: `cppgm++ --emit-lowir`

## Goal

Lower the PA12-resolved procedural subset (namespace functions, scalar/pointer
globals, statements, expressions) into PA13 LowIR text. The PA12 semantic pass
stays the source of truth; PA14 adds a lowering pass that consumes the typed
semantic tree, never the printed dump text.

## Ownership Boundaries

- `dev/src/sema/` (extended, behavior-preserving for PA11/PA12 fixtures):
  - `SemNode` gains typed lowering facts filled during analysis: resolved
    entity identity (`entity_scope` + `entity_name`, keyed by the declaring
    `Scope*`, which is stable), decoded constant values (`has_value` +
    `ConstValue`), string-literal bytes, null-pointer-literal marking, and
    declaration flags (static/extern/c-linkage) on variable/function items.
    The PA12 dump printer is unchanged; new fields are dump-invisible.
  - New statement support needed by the PA14 test surface and absent from
    PA12's subset: `goto`/labeled statements (new `SN_GOTO_STATEMENT` /
    `SN_LABEL_STATEMENT` kinds), default arguments (recorded per overload on
    `ScopeBinding`, expanded into synthesized argument nodes at call sites),
    `= delete` on namespace-scope functions (participates in overload
    resolution; selecting it is an error; never emitted), qualified
    namespace-scope function definitions (`int N::f(...) { }`), and
    `extern "C"` linkage marking. None of these appear in PA11/PA12 fixtures
    (verified by grep), so earlier stage outputs are unaffected.
  - `SelectBestOverload` accepts an optional per-candidate minimum arity so
    trailing default arguments make fewer-argument calls viable.
- `dev/src/lowering/` (new): the C++ -> LowIR lowering pass. It reads
  `SemUnit` trees plus the `TypesModel` scopes and writes LowIR text. It owns
  symbol naming, LowIR type mapping, conversion spelling, constant folding of
  global initializers, and function-body lowering. It does not re-run name
  lookup or overload resolution (identities and types come from the tree); it
  does call the shared typed helpers (`UsualArithmeticConversions`,
  `TypeSize`, `RenderConstValue`) for facts PA12 established once.
- `dev/cppgm++.cpp`: `--emit-lowir [-O0]` runs the same parse+bind pipeline as
  `--emit-semantics`, keeps the models alive, runs the lowering pass over all
  translation units, and writes one concatenated LowIR program.

## Module Layout (dev/src/lowering/)

- `lower_types.{h,cpp}`: C++ type -> LowIR spelling (`i32`, `u8`, `ptr`,
  `obj<NxA>`, enum -> `iN` of underlying size), scalar conversion-op selection
  (sext/zext/trunc/copy/sitofp/uitofp/fptosi/fptoui/fpext/fptrunc), predicate
  signedness.
- `lower_name.{h,cpp}`: LowIR symbol names (qualified path joined with `__`,
  overload index `__ovN` from the declaration-ordered overload set, shadowed
  local slots `name__shadowK`, `__strlit__N`), plus the Itanium mangler for
  the PA14 subset (nested names, builtin types, pointers, references, enums,
  substitutions, operator-function terminals) feeding `object=` metadata.
- `lower_const.{h,cpp}`: constant evaluation over analyzed init trees for
  global initializers: integer/enum folding, address constants
  (`addr @g [+ byte-offset]`), string-literal items, null -> `zero`.
- `lower_unit.{h,cpp}`: program assembly: entity registry across translation
  units, demand-driven `declare` entries, global definitions (scalar and
  structured array forms with `zero` tails), function emission order, and the
  canonical top-level phase order.
- `lower_function.{h,cpp}`: per-function state and statement lowering: slot
  table (params first, then creation order), label counter and the per-
  statement label allocation orders, block list in open order, break/continue
  targets, switch case pre-scan and dispatch, goto label map, implicit
  returns, dropping empty unreferenced trailing blocks.
- `lower_expr.{h,cpp}` (+ `lower_expr_ops.cpp` if needed for size): expression
  lowering in four contexts: value, address, branch condition, effect.
  Includes call lowering (direct, function pointer/reference with `as`
  signatures, reference-argument materialization `refarg__N`), short-circuit
  lowering (direct in condition context; `lor__N`/`land__N` i64 slots in
  value context), conditional lowering (`cond__N` value form vs
  `condaddr__N` address form chosen by use context), pointer arithmetic
  (byte-scaled `index i8` with explicit `mul`, element-typed
  `index <ty> [projection=array_element]` for subscripts), and the
  conversion-spelling rules below.

## Conversion / Immediate Spelling Rules (from the checked-in oracle)

- Non-immediate values: same-width integral re-spelling emits
  `copy <dst>` only when the LowIR spelling changes; width changes emit
  `convert <op> <dst> <src> <val>`; explicit casts always emit (`copy` even
  for identical spellings).
- Integral immediates: same-width conversions retype silently everywhere.
  Widening/narrowing canonicalizes to the final typed literal in
  copy-initialization contexts (variable init, call arguments, return,
  conditional arms) but is spelled as `convert` in operand contexts (usual
  arithmetic conversions, comparisons, plain and compound assignment).
- Floating conversions are always spelled, immediates included.
- Null pointer immediates materialize as `copy ptr nullptr` in value
  contexts and `zero` in global data.
- `sizeof` always materializes `const i64 <size>`.
- Truth tests: branch directly on integer/pointer/bool values; floats use
  `cmp ne <fN> v, 0.0`; `!v` uses `cmp eq i64 v, 0`; the stored rhs of a
  value-context `&&`/`||` normalizes through `cmp ne i64 v, 0`.

## Validation Plan

- `make -C pa14 test` (and `make test-report ACTIVE_TEST_REPORT_PAS='pa14'`)
  against the checked-in LowIR oracle, which validates structure and compares
  bodies near-exactly (temps, labels, slots) after metadata relaxation.
- Full `make test-report-through-pa14` to prove PA1-PA13 behavior is
  preserved (the sema extensions are additive; PA11/12 fixtures avoid the
  newly supported forms).
- `perl scripts/cppgm_file_audit.pl --stage pa14 --paths dev/src`.
- Spot-check the runnable scaffold on a couple of tests:
  `cppgm++ --emit-lowir` -> `lowir2cy86` -> `cy86`.

## Architecture Review

The implementation matches the plan's ownership story: `dev/src/sema/`
analyzes once and stamps typed lowering facts onto `SemNode`
(entity_scope/entity_name identity, decoded constants, string bytes,
declaration flags), and `dev/src/lowering/` consumes only that typed
state plus the scope model. The lowering's only lookups are
`FindOwnBinding` keyed by the resolved (scope, name) identity — direct
keyed access, not re-resolution — and the shared typed helpers
(`UsualArithmeticConversions`, `TypeSize`, `RenderConstValue`).

Review findings, fixed during the audit (details in `audit.md`):

- The Itanium mangler recovered enum/class scope paths by parsing
  `NamedTypeInfo::display` text. `NamedTypeInfo` now records its
  declaring scope and bare name structurally at creation, and the
  mangler walks those.
- The cross-translation-unit entity registry keyed identity on the
  "__"-joined display path, which is ambiguous (`a::b` vs an `a__b`
  identifier) and merged unnamed-namespace entities across units.
  Identity now uses a "::"-joined key with unnamed namespaces keyed by
  their per-unit scope object; symbol *spelling* still uses the "__"
  path with `UniqueSymbol` disambiguation.
- The registry containers are deques and global definitions render onto
  the entry itself, so references handed out while lowering (which can
  register new demand-driven entries) stay valid.

## Final Architecture Review

- One writer per concern: `LowerProgram` owns entity identity, naming,
  and top-level emission; `FunctionLowerer` owns per-body block/slot/
  label state; `lower_types` owns every C++->LowIR type and conversion
  spelling; `lower_const` owns global-initializer folding. No facts are
  derived from printed text anywhere in the pass.
- Unsupported constructs throw `OutsideBoundary` and the driver exits
  with failure: there are no fallback success paths, canned outputs, or
  test-shape gates, and `AddUnit` rejects synthesized class-helper
  output so the stage stays purely procedural per the README.
- Costs scale with program size: map-keyed registries, one pre-scan per
  switch subtree, per-mangling substitution tables, and string builds
  that append in place.
- PA15+ extension points are the ones the plan promised: the entity
  registry (add class members/helpers), `FunctionLowerer`'s expression
  walker (member access, this), and the centralized type/conversion
  spelling layer — all keyed off the same structural scope/type state,
  which now also carries named-type identity for mangling growth.

## Later-Stage Fit

- The lowering keys everything off resolved scope/type state, so PA15+ can
  add class lowering by extending the same entity registry and expression
  walker rather than replacing them.
- LowIR text emission is centralized (one writer), so PA28 `lowir2native`
  consumes the same canonical output.
