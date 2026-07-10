# PA24 Plan — First-Tier Core-Language Closure (`cppgm++ --emit-lowir`)

PA24 extends the PA14–PA23 source-to-LowIR path with the remaining ordinary
C++11 features: `auto` deduction (variables and visible-body returns), direct
braced initialization, captureless-plus-by-ref lambdas, and range-for. No new
output format; the checked-in `.ref` files under the relaxed LowIR comparator
are the contract.

Status: complete — 94/94 pa24 tests and the full through-pa24 suite
(2373/2373) pass; the pa24 file audit is clean.

## Ownership boundaries

- `dev/src/ast/` owns syntax only. Range-for gets a syntax representation
  (`for_range_decl` + `for_range_init` on `SK_FOR`); `EK_LAMBDA` already
  exists. No semantic decisions in the AST layer.
- `dev/src/sema/` owns all deduction and desugaring as typed facts:
  - `auto` deduction happens where the declaration/return composes its type,
    before any binding is created — downstream (lowering, mangling, constexpr)
    must only ever see the deduced type.
  - Range-for desugars during binding into the ordinary loop node shape from
    6.5.4 [stmt.ranged] (hidden `__range`/`__begin`/`__end` bindings), so the
    lowering layer sees only constructs it already knows.
  - Lambdas synthesize a real `ClassInfo` closure class (member `operator()`,
    captureless conversion to function pointer) through the existing class
    machinery, so calls/copies/lowerings reuse the ordinary member paths.
- `dev/src/lowering/` should need no new IR concepts; PA24 only widens which
  source reaches the existing lowering.

## Work plan (in order)

1. **Pointer/integer casts** (`sem_cast.cpp`): `reinterpret_cast`,
   functional-cast, and c-style casts between the supported pointer and
   integer forms.
2. **Direct braced initialization** (`sem_binder`/`sem_expr`): `int x{3}`,
   `int{3}`, braced string-literal array forms, braced array reference
   arguments.
3. **`auto` variables**: single-declarator with initializer, cv/`auto*`/
   `auto&`/`auto&&` declarators. Deduce by the template-argument rules
   (deduce as `T`/`T&`/`T&&` against the initializer), then flow through the
   ordinary declaration path with the deduced type.
4. **`auto` returns**: non-template functions and member functions with
   visible bodies, including ref-qualified `auto&&`. Deduce from the first
   `return` statement while binding the body; the composed function type is
   rewritten before the binding/mangling of the definition.
5. **Aggregate ctor arity** (ref-shape fix): the synthesized field-wise
   aggregate constructor takes one parameter per *provided* initializer
   (`Pair{a}` → `_ZN4PairC1Ei`), zero-filling omitted trailing scalar members
   inside the helper body. Omitted class-typed members keep a parameter and
   get a value-initialized temporary materialized at the call site
   (the `200-aggregate-omitted-class-tail` ref shape). The helper is keyed
   per (class, arity) so different uses can coexist; the aggregate-array path
   keeps its current full-arity shape unless a ref pins otherwise.
6. **Range-for**: AST fields + parse; sema desugars per 6.5.4 into
   `{ range/begin/end bindings; ordinary for }` for arrays, braced-init lists
   (materialized as a hidden bounded array), and member/ADL `begin`/`end`.
7. **Lambdas** (as landed, the hybrid reference model): every lambda
   synthesizes a closure class whose fields are the by-reference captures
   (reference fields) and the captured `this` (a pointer field), created on
   first use — spelled captures materialize unconditionally. `operator()`
   binds through the shared statement path. A captureless lambda
   additionally synthesizes an internal free function holding the same
   body: the target of the closure's function-pointer conversion (a
   standard-rank conversion, no call emitted), of plain-`auto` deduction
   (pointer at block scope, function type at namespace scope, closure kept
   for cv-`auto` and local-type-owning bodies), and of direct
   lambda-expression calls. Capture rewriting hooks: TryCaptureUse /
   ThisValueNode gate on the innermost frame whose scope is the open body.
8. **Misc cluster**: by-value indirect param copy shape, conversion-operator
   inc/dec + using-alias cases, conditional ctor conversion, dependent
   validation tests — diagnose after the main features land (several are
   expected to fall out of items 3–7).

## Validation

- Iterate per cluster with `make check TEST=...` in `pa24/`, then
  `make test-report ACTIVE_TEST_REPORT_PAS='pa24'`.
- After each cluster that touches shared sema/lowering, run the full
  `make test-report-through-pa24`; regressions in earlier stages are
  treated as part of the cluster's bug.
- `perl scripts/cppgm_file_audit.pl --stage pa24 --paths dev/src` before
  committing; commit per cohesive cluster.

## Architecture Review

Audit of the landed implementation (loop 55) against the ownership
boundaries above; full detail in `audit.md`.

- **Layering held.** The AST layer carries only syntax for the new
  forms: `ParseRangeForForm` stores the range declaration and
  initializer unanalyzed; all desugaring lives in
  `sem_auto.cpp::BindRangeForStatement`, which builds ordinary
  synthesized AST fragments and binds them through the shared variable
  and statement paths. Lowering never sees `auto`, an undesugared
  range-for, or `EK_LAMBDA`; the lambda expression is consumed entirely
  in sema (`AnalyzeLambda`).
- **Typed facts throughout.** The captureless closure's function
  identity is a typed record (`closure_functions_`, keyed by the class
  entity) queried through a hook by the conversion classifier
  (`closure_to_pointer` at exact rank) and applied in sema; lowering
  does not recompute it. No new name-string comparisons encode
  semantics: `__range`/`__begin`/`__lambda` strings are generated
  identities, never matched against.
- **One deviation found and fixed:** `LowerClosureInit` re-derived the
  closure field offsets positionally (`i * 8`) while sema had already
  recorded the authoritative offsets via the shared `LayoutField`
  machinery. Construction now reads the class record's field offsets
  (`LowerProgram::ProgramClass`), matching how the capture-read path
  already consumes `member_offset`.
- **One semantic corner found and fixed:** the plain-identifier range
  shortcut (which matches the reference's `__range`-free shape) broke
  when the loop variable shadowed the range name (`for (int a : a)`);
  the shortcut now yields to the hidden `__range` binding exactly when
  the names collide, so no pinned ref shape changes.
- **Reviewed and accepted:** `DeduceAutoDeclared`/`MatchAutoPattern`
  implement the 7.1.6.4p6 deduction rules directly instead of routing
  through `template_deduce.cpp`'s bound-slot machinery. The deduced
  type has a single owner (every auto context funnels through
  `DeduceAutoDeclared`); reuse would need an invented-type-param shim
  plus a carve-out for the namespace-scope function-view rule that
  `DecayForDeduction` cannot express, a net complexity loss. Also
  accepted: `SN_CLOSURE_INIT` is a sema-node construction shape (field
  -wise pointer stores pinned by the refs), not a new LowIR concept —
  the LowIR grammar is unchanged.

## Final Architecture Review

Post-fix state: sema owns deduction, desugaring, capture layout, and
the closure-function fact; lowering consumes recorded offsets and typed
node facts only; the AST stays syntactic. The hybrid captureless model
(internal function + closure class, both bound from the same lambda
body, emitted on demand through the weak-flag sweep) is a
reference-pinned design cost — bounded at two binds per captureless
lambda, cached per (lambda, enclosing body). Capture hooks are O(1)
outside lambda bodies and capture-count bounded inside; the per-arity
aggregate constructor is memoized per cover; no program-wide walks,
timeouts, or fallback success paths were added. Gates after the audit
fixes: pa24 94/94, through-pa24 2373/2373, file audit clean.
