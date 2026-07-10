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
