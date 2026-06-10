# PA12 Audit

## Audit Plan

Scope: commit dd052c1ee (`Implement PA12 emit-semantics`) against
`pa12/plan.md` and `pa12/README.md`. 126 tests (9 expected-failure);
`make test-report-through-pa12` green at intake.

Files to inspect:

- New: `dev/src/sema/sem_node.{h,cpp}`, `sem_convert.{h,cpp}`,
  `sem_expr.{h,cpp}`, `sem_binder.{h,cpp}`.
- Shared-surface diffs (regression risk for PA7-PA11):
  `sema/type.{h,cpp}`, `sema/type_builder.{h,cpp}`, `sema/scope.{h,cpp}`,
  `sema/decl_binder.{h,cpp}`, `parse/parse_token.{h,cpp}`,
  `ast/ast.cpp`, `ast/ast_expr.h`, `ast/ast_parse_expr.cpp`,
  `ast/ast_parse_stmt.cpp`, `dev/cppgm++.cpp`,
  `dev/frontend_source_sets.mk`.

Cheating checks:

- No test-name/fixture-content gates, no `.ref` consultation, no
  source-shape special cases keyed to test inputs (initial grep clean;
  verify hot paths by reading).
- `--emit-semantics` must run real analysis (phases 1-7, PA10 parse,
  PA11 binding, expression/conversion/overload work) — not canned
  output, not an interpreter/template substitute.
- Failure paths must come from real semantic rejection, not blanket
  accept/reject of constructs by spelling.

Ownership/stringly checks:

- Canonical callee names: derived from scope structure
  (`CanonicalQualifiedName`) rather than string concatenation at parse
  time?
- Literal facts: typed (`EPostTokenKind`/`EFundamentalType`/value
  bytes) on `ParseToken`/`AstExpr`, no spelling re-lexing downstream?
- Value categories/types as enums/`TypePtr` in `SemNode`, with the
  printer consuming typed fields only?
- Qualified type displays: selected by the PA12 binder via a seam, not
  forked display logic duplicated in the model?
- Overload sets: owned by `ScopeBinding`; check the PA11 binder is
  unaffected and PA12 set maintenance is single-sourced.

Performance risks:

- `SelectBestOverload`: candidates x args classification — fine; check
  no repeated reclassification per call site.
- `CanonicalQualifiedName` walks owner chains per callee; check it is
  not called inside loops over scopes (depth is tiny; acceptable).
- Scope lookup in expression analysis: ensure lookups use the existing
  map-based scope lookup, not linear rescans of declarations.
- Type printing (`DescribeType`) per node print — once per dump line;
  acceptable. Check `TypeEquals` recursion isn't quadratic on deep
  types in conversion ranking loops.
- Per-unit work: one forward pass claimed; confirm no second full-tree
  walk besides the printer.

File-audit checks:

- `perl scripts/cppgm_file_audit.pl --stage pa12 --paths dev/src` must
  pass; confirm no oversized files dodged by mechanical splits, no
  implementation hidden in headers to dodge per-file limits, no
  unchecked paths gaining logic.

Regression checks:

- PA10 dump unaffected by `ast_parse_expr` changes (literal fact
  stamping, multi-keyword functional casts, bare decltype roots).
- PA11 dump unaffected by `decl_binder` seam restructuring and
  `scope`/`type`/`type_builder` extensions (8.3.5p5 mode off for PA11).
- PA6-PA9 untouched surfaces; `make test-report-through-pa12` is the
  gate.

## Findings

1. **Stringly ctor-name recovery (fixed).**
   `SemBinder::EnsureDefaultConstructor` recovered the qualified
   constructor name by re-parsing the entity's display spelling
   (`info->display.substr(find(' ')+1)` plus `rfind("::")`). The
   qualified name is a structural fact the model already holds: the
   class's member scope records the entity name and its enclosing
   scope chain. Display strings are for output; deriving identity
   facts from them is downstream recovery the model should provide.

2. **No cheating found.** `--emit-semantics` runs the full pipeline
   (phases 1-7, PA10 parse, PA11 binding, expression/conversion/
   overload analysis) per unit; worker-thread failures rethrow in
   `run_unit_on_large_stack`, so there is no fallback success path.
   No test-name, fixture-content, or source-shape gates exist;
   `__builtin_constant_p` is recognized by name only after value
   lookup fails (it is a builtin), and the `T()`/enumerator literal
   folds are the dump semantics the fixtures pin, computed from typed
   facts, not canned strings. The printer consumes only typed `SemNode`
   fields.

3. **No regressions in shared surfaces.** The `DeclBinder` seam
   restructure keeps PA11 behavior in the base class (no-op events,
   merge-or-error function binding, unqualified displays).
   `TK_MEMBER_POINTER`, function-type cv, and the 8.3.5p5 adjustment
   are new forms/modes that no PA7-PA11 fixture exercises; PA10's
   parser changes only add accepts (multi-keyword casts, bare decltype
   roots, labeled declarations). Through-pa12 report green confirms.

4. **No performance blockers.** One forward traversal per unit; name
   lookups go through map-backed `binding_index`
   (`FindOwnBinding`/`UnqualifiedLookup`); `SelectBestOverload` is
   O(candidates x args) with the standard O(viable^2) ambiguity
   verify; printing is one pass over the recorded tree. No
   suite-level rescans, no hot-path recomputation. The
   `DirectiveClosure` per unqualified lookup is pre-existing PA11
   machinery, fine at PA12 scope sizes.

5. **File audit passes.** One pre-existing warning
   (`parse/parser.h` implementation-in-header) is PA6 surface used
   only by the `recog` driver - flagged since PA6, not part of the
   PA12 change set, and not a bypass (the audit tool sees and reports
   it).

6. **Minor, accepted.** `StripParens`/`OutsideBoundary` tiny helpers
   appear in both `sem_expr.cpp` and `sem_binder.cpp` anonymous
   namespaces; `sem_convert`'s `DecayedValueType` and `sem_expr`'s
   `DecayToPointer` are distinct operations (full 4.1-4.3 value
   transformation vs. array decay for operand typing) despite similar
   names. Neither duplicates ownership of a semantic fact.

## Changes Made

- `dev/src/sema/sem_binder.{h,cpp}`: added
  `SemBinder::QualifiedScopePath(const Scope*)` - the named
  namespace/class path walk - and used it both in `TypeDisplayName`
  (replacing its inline loop) and in `EnsureDefaultConstructor`, which
  now builds the constructor name from the class's member scope
  (`model_.MemberScope(info)->name` + enclosing path) instead of
  parsing `info->display`.

## Validation

- `make -C pa12 test`: 126/126 after the change.
- `make test-report-through-pa12`: 694/694, all 12 stages green
  (PA1-PA11 regression gate for the shared surfaces).
- `perl scripts/cppgm_file_audit.pl --stage pa12 --paths dev/src`:
  passes (one pre-existing PA6 warning, see Findings 5).
