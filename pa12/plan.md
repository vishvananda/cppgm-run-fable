# PA12 Plan: `cppgm++ --emit-semantics`

## Goal

Add the PA12 call-semantics layer on top of the PA10 AST and the PA11
scope/type model: resolved expression analysis with value categories,
the limited standard-conversion subset, non-template overload
resolution, statement/scope semantics for function bodies, and the
deterministic semantic dump gated by the checked-in `.ref` fixtures.

## Pipeline

`run_emit_semantics_mode` in `dev/cppgm++.cpp`:

1. For each srcfile, run translation phases 1-7 and parse with the PA10
   `AstParser` (existing large-stack worker thread).
2. Run the PA12 semantic binder (`SemBinder`, extending the PA11
   `DeclBinder`) over each unit. One forward traversal builds the PA11
   scope/type model *and* a per-TU semantic dump tree (`SemNode`s) in
   declaration order, analyzing every function body as it is bound.
3. Print all units: `<n> translation units`, then per unit
   `start translation unit <k>` / `translation-unit` + tree /
   `end translation unit`.

Any pipeline, parse, or semantic error exits `EXIT_FAILURE`.

## Output format facts pinned by the fixtures

- Two-space indentation; items at depth 1 under `translation-unit`.
- Top-level items in declaration order: `type-alias <name> <type>`,
  `variable <name> <type>` (initializer subtree nested, absent when
  uninitialized), `function-declaration <name> <type>`,
  `function-definition <name> <type>`, `namespace-definition <name>`
  (`<unnamed>` for unnamed; reopened namespaces print one node per
  appearance). Class/enum declarations print nothing at namespace
  scope.
- `<name>` of functions in items and `callee` lines is the canonical
  qualified name: named namespace path from the global scope (unnamed
  namespace components are skipped), e.g. `outer::inner::value`.
  id-expressions print the name as written (`::f`, `wrap::g`, `N::value`).
- `function-definition` children: one `parameter <name> <type>` per
  parameter (name may be empty - the line keeps the double space), then
  the body `compound-statement`. PA12 function types apply the 8.3.5p5
  parameter adjustment (`char*[]` -> `pointer to pointer to char`),
  unlike the PA11 dump, which pins unadjusted declared types.
- Statements: `compound-statement`, `simple-declaration` (wrapping each
  block-scope declaration statement, possibly empty, e.g. a local enum
  definition), `expression-statement`, `return-statement`,
  `if-statement` (`condition` / `then` / `else`), `while-statement`
  (`condition`, body), `do-statement` (body, `condition`),
  `for-statement` (`for-init-statement`, `condition`, `iteration`,
  body), `switch-statement` (`condition`, body), `case-statement`
  (value expression + nested labeled statement), `default-statement`,
  `break-statement`, `continue-statement`. Block-scope alias
  declarations print `type-alias` directly (no wrapper); block-scope
  using-declarations/directives/namespace-aliases print nothing.
  Condition declarations print `condition` / `condition-declaration` /
  `variable` and scope over the dependent statements.
- Expressions print `<node> <value-category> <type> [annotation]` with
  resolved facts:
  - `literal prvalue <type> <spelling>` (source spelling; keyword
    literals as `KW_TRUE:true`, `KW_NULLPTR:nullptr`); string literals
    are `literal lvalue array of N const char "..."`.
  - Enumerator id-expressions print as `literal prvalue <enum-type>
    <decimal-value>`; `T()` of an integral/enum type prints
    `literal prvalue <type> 0`; `__builtin_constant_p(e)` (when no user
    declaration exists) folds to `literal prvalue int 0|1`.
  - A literal null pointer constant initializing/passed/returned/
    assigned to a pointer or `nullptr_t` destination is *retyped* in
    place (`literal prvalue pointer to int 0`); operands of built-in
    operators are not retyped.
  - `id-expression <vc> <type> <name-as-written>`: reference types are
    stripped; variables/parameters are lvalues; the declared type keeps
    its cv (`id-expression lvalue const int x`).
  - `call-expression <vc> <type>`: type is the *raw* return type
    (references kept: `call-expression lvalue lvalue-reference to int`),
    vc from the return type (& -> lvalue, && -> xvalue, else prvalue).
    A resolved named function prints a `callee <canonical-name> <type>`
    first child; calls through pointers/references/deref print the
    callee expression node itself. Arguments follow unconverted (except
    literal retyping).
  - `unary-expression` / `binary-expression` / `postfix-expression` /
    `assignment-expression` annotate `TOKEN:spelling` (`OP_XORASS:^=`).
    Parens are transparent. Commuted subscripts normalize: the
    array/pointer operand prints first.
  - `member-expression <vc> <type> OP_DOT:<member>`; injected
    anonymous-union member accesses print without the operator
    annotation and synthesize the storage-variable id-expression child.
  - `cast-expression <vc> <type>` with `OP_LPAREN:` (C-style, empty
    spelling), `KW_STATIC_CAST:static_cast`, or no annotation
    (functional casts, including multi-keyword and decltype forms).
    A cast to a reference type prints no cast node: the operand node's
    category/type are adjusted in place (xvalue + reference type).
  - `sizeof-expression prvalue unsigned long int` is a leaf for both
    forms.
  - `conditional-expression <vc> <type>` with three children;
    `braced-init-list lvalue <type>` wraps array initializer elements.
- Default-initialized class-typed variables print a
  `constructor-action <Q>::<C>` child wrapping a synthesized
  `call-expression prvalue void` of the implicit default constructor
  applied to `&var`; one synthesized
  `function-definition <Q>::<C>::<C>(pointer to <class>) -> void` with
  `parameter this` and an empty body is appended per class at the end
  of the unit, in first-need order.
- Local anonymous class types used by a declarator are renamed
  `__local_type<n>` (TU-wide counter); standalone block-scope anonymous
  unions keep the PA11 span name and synthesize the
  `__anonymous_union_storage__<b>_<e>` variable.
- PA12 named-type displays are namespace-qualified (`struct n::S`);
  the PA11 dump keeps unqualified displays, so the qualified spelling
  is selected by the PA12 binder, not hard-wired into the model.

## Ownership boundaries

- `dev/src/sema/type.*` (shared, extended): `TK_MEMBER_POINTER` (class
  info + member type, cv on the node) and function-type cv-qualifiers
  (`function of () const returning int`) for the member-pointer tests.
  `DescribeType`/`TypeEquals`/`TypeSize`/`TypeAlignment` learn the new
  forms; nothing else changes, so PA7-PA11 behavior is preserved (no
  earlier fixture exercises these forms).
- `dev/src/parse/parse_token.*` (shared, extended): `ParseToken` keeps
  the post-token literal facts (kind, fundamental type, element count,
  value bytes) so semantic passes consume typed state instead of
  re-lexing spellings. `ast/ast_parse_expr.cpp` stamps them into
  `EK_LITERAL` nodes; `ParsePostfixRoot` learns multi-keyword
  functional casts (`unsigned long(e)`), storing the keyword list on
  the node. The PA10 dump ignores all new fields (no PA10 fixture
  contains a multi-keyword cast).
- `dev/src/sema/type_builder.*` (shared, extended): composes
  `DI_MEMBER_PTR` prefixes and function cv suffixes; an
  `adjust_parameters` mode (off for PA11) applies 8.3.5p5 to function
  types. The PA11 callers construct it exactly as before.
- `dev/src/sema/scope.*` (shared, extended): `ScopeBinding` gains its
  owner scope (stamped in `AddBinding`; powers canonical names), an
  overload type list used only by the PA12 binder, and the
  anonymous-union storage association. The PA11 printer and binder
  ignore the new fields.
- `dev/src/sema/decl_binder.*` (shared, restructured): the traversal
  gains protected virtual seams - type display naming, anonymous type
  naming, function name binding (PA11: merge-or-error; PA12:
  overload-aware), namespace body binding, type-alias/variable/
  anonymous-union events, statement binding, and decltype resolution.
  Base behavior is unchanged for PA11.
- `dev/src/sema/sem_node.*` (new): the PA12 semantic dump tree
  (kind-tagged nodes holding name/type/value-category/operator facts)
  and its deterministic printer.
- `dev/src/sema/sem_convert.*` (new): conversion classification for the
  PA12 subset (identity, lvalue-to-rvalue, array/function decay,
  cv-stripping, integral/floating promotions and conversions,
  pointer/bool/nullptr conversions, qualification conversions, the
  supported reference bindings), implicit-conversion-sequence ranking
  (exact/promotion/conversion/ellipsis plus the 13.3.3.2 reference and
  pointer-vs-bool tie-breakers), and best-viable-function selection.
- `dev/src/sema/sem_expr.*` (new): expression analysis over `AstExpr`
  producing `SemNode`s + (type, value category) facts: literals,
  id-expressions (including overload sets and target-directed
  resolution), calls, built-in operators, casts, sizeof, member access,
  braced initializers, and the copy-initialization checks shared by
  variables/arguments/returns/conditions. Reaches the binder through a
  narrow context interface (scope lookup, const-expr evaluation,
  type-id resolution, constructor synthesis).
- `dev/src/sema/sem_binder.*` (new): `SemBinder : DeclBinder`
  overriding the seams: records dump items in declaration order,
  analyzes initializers and function bodies (statement walk with
  condition/for scopes), maintains the `__local_type` counter, seeds
  `nullptr_t`, performs the 8.3.5p5 adjustment mode, synthesizes
  implicit default constructors, and extends decltype to general
  expressions through the expression analyzer.
- `dev/cppgm++.cpp`: `--emit-semantics` wiring only.

## Semantic decisions (assignment boundary)

- Overload sets exist only for functions sharing one scope binding;
  using-declarations import the set; later declarations in the same
  scope extend it. Resolution considers the set visible at the call
  point (one forward pass), per the reopened-unnamed-namespace fixture.
- ICS ranks: Exact (identity, lvalue-to-rvalue, decay, qualification),
  Promotion, Conversion, Ellipsis; ambiguity or no viable candidate
  throws -> `EXIT_FAILURE`. Reference binding follows the basic 8.5.3
  cases: identity/qualification-compatible direct binding only; rvalue
  references do not bind lvalues; binding never loses qualifiers.
  Tie-breakers: rvalue references prefer rvalues, lvalue references
  prefer function lvalues, less cv-qualified referees win, and
  pointer-to-bool loses to other pointer conversions.
- Built-in operators implement the procedural subset: usual arithmetic
  conversions (unscoped enums promote), pointer arithmetic and
  comparisons (composite pointer types, null pointer constants),
  logical/conditional/comma/assignment/compound assignment over
  modifiable lvalues, subscripts, inc/dec, unary operators, and the
  supported cast targets (arithmetic/enum/pointer/nullptr/void).
- Anything outside the boundary (member function calls, overloaded
  operators, user-defined conversions, templates, goto/throw/try,
  general braced initialization) throws -> `EXIT_FAILURE`.

## Validation

- `make test-report ACTIVE_TEST_REPORT_PAS='pa12'` while iterating;
  `make -C pa12 check TEST=tests/...` for single cases.
- Root `make test-report-through-pa12` (the exit criterion) to prove
  PA1-PA11 behavior is preserved. The shared regression surface is
  `sema/type.*`, `sema/type_builder.*`, `sema/scope.*`,
  `sema/decl_binder.*`, `parse/parse_token.*`, and the AST expression
  parser; PA10 (`--emit-ast` dump) and PA11 (`--emit-types` dump,
  nsdecl/nsinit) cover it.
- `perl scripts/cppgm_file_audit.pl --stage pa12 --paths dev/src` for
  the architecture gate.
