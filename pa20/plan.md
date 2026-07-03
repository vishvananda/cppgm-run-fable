# PA20 Plan: Full Constant Evaluation (`cppgm++ --emit-lowir`)

## Goal

Turn the PA11/PA19 integral constant-expression subset into a complete
compiler-owned constant-evaluation layer: `constexpr` functions
(recursion, locals, loops, assignment), `constexpr` constructors and
member functions, object/array/pointer/reference-valued constants,
floating-point evaluation, `noexcept` constant expressions, constant
initialization of globals, and function-local statics (constant and
guarded-dynamic). PA20 still emits PA13 LowIR; the lowering surface
grows only where the checked-in refs demand it.

## What the fixtures pin down (oracle findings)

- Scalar `constexpr` globals fold into the LowIR global initializer
  (`global @k : i32 ... = 6`) and the folded-away `constexpr` helper
  functions are *not emitted* (constexpr functions are implicitly
  inline, 7.1.5p2, hence weak/demand-emitted; the folded initializer
  demands nothing).
- Uses of such variables still `load` the global; only the
  initializer folds. Class-typed `constexpr` globals keep today's
  dynamic-init shape (`{ zero N }` plus `__cppgm_init` actions) —
  identical to non-constexpr class globals. The constant value is a
  sema-side fact for `static_assert`/template arguments, not a new
  lowering shape.
- Scalar float globals render numerically (`f64 ... = 3`, not the
  source token); structured float array items keep the source token
  (`f80 1e0L`). No earlier-PA ref constrains the scalar rendering.
- Instantiated `static constexpr` class/array data members, when
  odr-used at runtime, emit as weak globals with a **flattened
  constant image** (`u16 0, u16 0, u16 171, ...`) and no dynamic
  init. This is the one genuinely new global-emission form.
- Function-local statics hoist to internal globals named
  `__local_static__<fn>__<var>__tokens<A>_<B>` (A = first token of the
  init-declarator, B = terminating `;`, 0-based over the
  translation-unit post-token stream; global names are compared
  byte-exactly by the harness). Shapes:
  - scalar with constant initializer: constant global init, no guard,
    no code at the declaration;
  - everything else (arrays, class objects, references, dynamic
    scalars): the global carries the namespace-scope image emission
    result (constant items where that path folds, `zero` otherwise)
    **plus** an `<name>__guard : i64` global and, at the declaration
    point, `load guard / cmp ne 0 / branch ^ready ^init`, the
    declaration's ordinary init actions addressed at the global, a
    `store i64 1` to the guard, `jump ^ready`. Labels allocate
    ready-then-init from the shared label counter.
- `noexcept(expr)` folds from declared/derived unwind facts: any
  potentially-evaluated call to a callee without a non-throwing spec
  makes it false (user-provided ctor without `noexcept` => false;
  defaulted/trivial ctor => true; `declval<const H&>()(1,2)` works in
  the unevaluated operand).
- The one rejection fixture: a class prvalue converted to `bool`
  through a **non-constexpr** conversion function used as an NTTP must
  fail. The engine only calls bodies marked constexpr (or
  compiler-synthesized special members).

## Architecture

### New module: `sema/const_eval` (the engine)

An interpreter over **analyzed SemNode trees** — never over raw AST
and never over printed text. The expression analyzer has already done
lookup, overload resolution, template deduction/instantiation,
conversions, member layout (byte offsets), and constant folding of
enumerators/sizeof; the engine only supplies evaluation.

Files (audit budget: <=1500/source, <=120/function):

- `sema/const_eval.h` — value model + engine interface.
- `sema/const_eval.cpp` — objects, typed scalar/pointer reads and
  writes, conversions, the body registry.
- `sema/const_eval_expr.cpp` — expression evaluation (rvalue, lvalue,
  init-into-object forms).
- `sema/const_eval_stmt.cpp` — statements, calls, constructor
  evaluation (member-init lists, value-init zero-fill, trivial
  copies).

Value model (typed constants, per the handout's design notes):

    struct ConstObject {            // one compile-time object
        TypePtr type;
        vector<unsigned char> bytes;             // scalar payloads
        map<offset, ConstPointer> ptr_slots;     // pointers by offset
    };
    struct ConstPointer {           // pointer/reference/lvalue value
        shared_ptr<ConstObject> object;  // engine-owned storage
        const Scope* sym_scope; string sym_name; // address constants
        unsigned long long offset;               // byte offset
    };                                           // all-null => nullptr
    struct EvalValue {              // one rvalue
        kind IN {INT, FLOAT, PTR};
        ConstValue ival; TypePtr float_type; long double fval;
        ConstPointer ptr; TypePtr type;
    };

The byte-image representation makes member access (`member_offset`),
array indexing, nested aggregates, `copyobj`-style trivial copies, and
pointer arithmetic uniform, and it round-trips into the flattened
LowIR global-image emission. References and `this` are just
`ConstPointer` frame slots — no separate reference machinery.

Evaluation core:

- `EvalRead(node)` -> EvalValue; `EvalLValue(node)` -> ConstPointer;
  `EvalInit(node, dest)` initializes the object at `dest`
  (constructor actions, braced lists, synth-copy wrappers,
  string-literal element lists).
- `ExecStatement` returns a control signal (normal/return/break/
  continue); frames hold (scope,name) -> ConstPointer bindings for
  parameters and locals plus the return target.
- Calls: the **body registry** maps a resolved SN_CALLEE to its
  SN_FUNCTION_DEFINITION. It scans `unit_.items/deferred/synthesized`
  incrementally and keys by the definition's canonical qualified
  `name` + `TypeEquals` on the (this-adjusted) signature, with
  `fn_spec` pointer identity taking precedence for template
  specializations — the same identity scheme the lowering uses.
- Constexpr gating: a body is callable iff its definition node is
  marked constexpr (new SemNode flag stamped from the declaration
  specifiers at every body-construction site) or is a
  compiler-synthesized/defaulted special member. This is what rejects
  the non-constexpr conversion-function NTTP fixture.
- Limits: call depth 512 (fixture needs 128), a global step budget,
  and hard errors for UB (division by zero, out-of-range access,
  reading indeterminate/foreign storage).
- Float arithmetic evaluates in the operand's common type on host
  float/double/long double (LP64 x86 matches the target model).

Ownership: the engine owns evaluated storage (`ConstObject`s) and the
**constant-object store** keyed by (declaring scope, name) — the
reusable evaluated-value storage for constexpr variables, static
constexpr members, and string literals. `ScopeBinding.has_value`
remains the integral fast path the PA11-PA19 machinery already reads;
the engine writes it back for integral scalars so template arguments,
enumerators, and array bounds keep working unchanged.

### Binder integration (`sem_binder`, `decl_binder`, `template_args`)

One virtual seam on DeclBinder:
`TryFullConstant(const AstExpr&, ConstValue&)` (default: false).
SemBinder implements it: analyze the expression with the existing
SemExprAnalyzer (normal evaluated context), evaluate with the engine,
convert to the integral/bool result the caller needs. Fallback sites
— each keeps the PA11 AST evaluator as the fast path and falls back
on failure:

- `static_assert` conditions (contextual-bool over the engine value;
  pointer results convert by non-nullness),
- array bounds,
- enumerator values,
- template value arguments and defaults (`template_args.cpp`), off in
  abstract pattern contexts (those stay dependent, as today),
- const-integral variable initializers (quiet fallback: folding these
  is an optimization, not a requirement).

Constexpr declarations:

- Variables (`OnVariableBound`/`AttachObjectLifetime` path): when the
  declaration is constexpr, evaluate the analyzed initializer.
  Success: store the ConstObject; write `has_value` for integral
  scalars; stamp the SN_VARIABLE for the lowering (folded scalar
  literal init for int/float scalars; flattened image for weak
  static-member class/array definitions). Failure: hard error
  (7.1.5p9). Non-literal types: error.
- Functions: `constexpr` joins `inline` in the weak/demand-emitted
  marking (7.1.5p2) at `BindFunctionBody`, member bodies, and
  specialization instantiation; every body-construction site stamps
  the definition node's constexpr flag.
- In-class `static constexpr` array members complete their bound from
  the braced initializer (9.4.2 + 8.5.1) so `sizeof` and pointer
  arithmetic over them work at class scope.

`noexcept(expr)` (5.3.7): parsed already (EK_TYPE_TRAIT / KW_NOEXCEPT);
analyze the operand as an unevaluated operand, fold to a bool literal
via the may-throw walk over the analyzed tree (the existing
`NodeMayThrow` logic, refactored to a shared free function over
SemNode so the analyzer can reach it).

### Lowering (`lower_unit`, `lower_function`, `lower_const`)

- Scalar folded initializers ride the existing `EvaluateLowerConst`
  paths (the binder stamps `has_value` / a float literal node; float
  scalars render via shortest round-trip so `3.0` emits as `3`).
- Weak `static constexpr` member definitions with a stamped constant
  image emit the flattened item list (typed scalar items in layout
  order, `zero` runs for padding, `ptr addr` for address slots) and
  suppress their dynamic-init actions.
- Function-local statics (`LowerLocalVariable` on `is_static_decl`):
  hoist to an internal global named from the stamped declarator token
  span, reuse the namespace-scope image emission for the global, and
  emit the guard shape above unless the declared type is a
  non-class/non-array/non-reference scalar whose initializer folds.
  Id uses inside the function resolve to the hoisted global through
  the program's (scope,name) global index. Local statics with
  destructors stay outside the boundary (unchanged error).
- Token spans: the AST records begin/end per init-declarator (new
  fields stamped by the declaration parser); the binder threads them
  onto SN_VARIABLE.

## Validation plan

- `make test-report ACTIVE_TEST_REPORT_PAS='pa20'` for the feature
  loop; each stage below should only add passes.
- Stages: (1) engine + static_assert/template-arg fallback (scalar
  results), (2) constexpr variables and object values incl. floats,
  (3) methods/ctors/references/pointers, (4) noexcept, (5) local
  statics + weak member images, (6) validation/rejection.
- `make test-report-through-pa20` after each stage that touches
  shared machinery (binder seams, weak emission, specifier handling);
  pa11-pa19 regressions are blockers — the AST evaluator stays the
  primary path everywhere it succeeds today.
- `perl scripts/cppgm_file_audit.pl --stage pa20 --paths dev/src`
  before committing.

## Handoff to PA21/PA22

The engine is a standalone typed layer over analyzed trees with a
narrow entry API (evaluate expression / initializer, constant-object
store, body registry). Template machinery consumes it through the
same fallback used by ordinary code, so PA21/PA22 can extend the
template model without touching evaluation again.
