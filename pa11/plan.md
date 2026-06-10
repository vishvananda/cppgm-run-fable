# PA11 Plan: `cppgm++ --emit-types`

## Goal

Add the PA11 semantic layer on top of the PA10 AST: scope formation,
declaration binding, name lookup, declarator-derived canonical types, the
supported integral constant-expression subset, and the deterministic
scope-tree dump gated by the checked-in `.ref` fixtures.

## Pipeline

`run_emit_types_mode` in `dev/cppgm++.cpp`:

1. For each srcfile, run translation phases 1-7 and parse with the PA10
   `AstParser` (existing `--emit-ast` path, large-stack worker thread).
2. Run the new PA11 binder over each translation unit's AST, producing a
   per-TU `Scope` tree (semantic model). Binding shares the worker thread
   because its recursion mirrors AST nesting.
3. Print all units: `<n> translation units`, then per unit
   `start translation unit <k>` / `translation-unit` scope dump /
   `end translation unit`.

Any pipeline, parse, or semantic error exits `EXIT_FAILURE`.

## Output format facts pinned by the fixtures

- Indentation is two spaces per level; the global scope prints as
  `scope namespace <global>` at depth 1 under `translation-unit`.
- Within a scope: all bindings print first (in first-binding order), then
  all child scopes (in creation order).
- Binding lines: `type` (class/enum names), `type-alias` (typedef/alias),
  `enumerator <name> <type> <value>`, `function`, `variable`, `parameter`.
- Namespace member scopes print as `scope namespace N` children; namespace
  aliases and using-directives print nothing.
- Using-declarations create a new printed binding in the current scope with
  the kind of the named entity (`type-alias Y int`, `type EY enum class EY`,
  `variable K const int`, `function f ...`, `enumerator A enum EY 7`).
- Function scopes print only for definitions; the body is a nested
  `scope block` child. Parameters bind (named ones only) in the function
  scope with their declared types.
- Function types do NOT apply the 8.3.5p5 parameter adjustment
  (`function of (array of 3 int) returning int` is canonical); `(void)`
  normalizes to `()`.
- Class scopes print only when the class is defined; a forward declaration
  is just the `type C struct C` binding.
- Scoped enums get a `scope enum E` child at their first declaration (even
  opaque); enumerators bind inside it. Unscoped enums print no scope and
  inject enumerators into the enclosing scope.
- Templates: a `scope template-parameters` child of the enclosing scope;
  type parameters bind as `type T typename T`, template-template
  parameters as `type TT template-parameter TT`, and the templated
  declaration's own name binds inside the template-parameter scope.
- Anonymous namespace-scope unions: the class scope is named
  `__anonymous_union_type__<b>_<e>` where `[b,e)` is the declaration's
  PA6-terminal token span (including the trailing `;`, pinned by the PA11
  and PA12 fixtures); members bind in the class scope and inject as
  variables into the enclosing scope. No `type` binding line.

## Ownership boundaries

- `dev/src/sema/type.*` (shared with nsdecl/nsinit, extended): adds the
  named-type kinds `TK_CLASS`, `TK_ENUM`, `TK_TYPE_PARAM`. Each named type
  points at one `NamedTypeInfo` record (canonical display spelling,
  completeness, size/alignment once complete) owned by the per-TU model;
  identity is pointer equality. `DescribeType`/`TypeEquals`/`TypeSize`/
  `TypeAlignment` learn the new kinds; everything else is untouched so the
  PA7/PA8 tools keep their behavior. The 7.1.6.2p3 simple-type-specifier
  combination table moves here as a shared free function used by both the
  PA7 `DeclParser` and the PA11 type builder.
- `dev/src/sema/scope.*`: the PA11 scope model and its printer. `Scope`
  (kind, name, parent, ordered bindings, ordered children, using-directive
  list) and `ScopeBinding` (print kind, name, type, optional constant
  value, optional target scope). A `TypesModel` arena owns scopes,
  `NamedTypeInfo` records, and the type-info -> member-scope map.
- `dev/src/sema/scope_lookup.*`: unqualified lookup (3.4.1 with the 7.3.4
  anchored, transitively-closed using-directive algorithm, ported from the
  PA7 design onto the new scope chain) and qualified lookup (3.4.3.2
  namespace traversal; class and scoped-enum member lookup). Read-only.
- `dev/src/sema/type_builder.*`: AstSpecifierSeq -> base type
  (simple-specifier combination, type-name lookup, decltype forms, nested
  class/enum specifiers via a binder callback), declarator composition
  (pointers/references/arrays/functions, nesting, cv distribution), type-id
  resolution for sizeof/alignof/casts/enum-bases/aliases.
- `dev/src/sema/const_expr.*`: the supported integral constant-expression
  subset over `AstExpr` (integer/char/bool literals, id-expressions naming
  enumerators or visible const integral variables with recorded constant
  initializers, unary/binary/conditional operators, static_cast/C-style/
  functional casts to integral types, `sizeof(type-id)`/`alignof(type-id)`,
  and `sizeof(id)` after semantic type disambiguation). It sees the binder
  only through a small context interface (name -> constant, name -> type,
  type-id -> TypePtr), so there is no dependency cycle.
- `dev/src/sema/decl_binder.*`: the AST traversal that owns declaration
  semantics: namespaces (reopen, inline/unnamed implicit directives,
  aliases, using-directives/declarations), simple declarations and
  typedefs/alias-declarations, class definitions/forward declarations and
  members, enums and enumerator evaluation, anonymous-union member
  injection, function definitions (function + block scopes), template
  declarations (parameter scopes), static_assert, and statement walking
  (block scopes and local declarations only).
- `dev/cppgm++.cpp`: `--emit-types` mode wiring only.

## AST changes (PA10 dump unaffected)

- `AstDecl` gains `begin_token`/`end_token` (PA6-terminal span), stamped in
  `ParseDeclaration`/`ParseMemberDeclaration`; feeds the anonymous-union
  mock name. The printer ignores them.
- `AstDecl` gains `enum_body`, set when an enum-specifier consumes `{`,
  so the binder can tell `enum class E {}` (definition) from
  `enum class E;` (opaque declaration). The PA10 dump never needed the
  distinction and is unchanged.

## Semantic decisions (assignment boundary)

- One model per translation unit; no cross-TU linking in PA11.
- Redeclarations merge into the existing binding (variables/functions/
  typedefs require mergeable types via `MergeRedeclaredType`; class
  forward/definition share one entity; opaque enum redeclaration must
  agree on scoped-ness and underlying type). No overload sets: a second
  function declaration with a different type is an error.
- Scoped enums are complete from their first (opaque) declaration with
  underlying type `int` unless a fixed base is given; unscoped opaque
  declarations without a base are ill-formed (7.2p2).
- `constexpr` objects are recorded as `const` for type and value purposes.
- Const integral variables with constant initializers record their value
  at the declaration; reading a non-constant in a constant context throws.
- Class layout (size/alignment) is computed at completion from non-static
  data members (union: max; struct: aligned sequential; empty: size 1) so
  `sizeof` works through the shared `TypeSize`; incomplete types throw.
- Out-of-boundary constructs the grammar accepts (member pointers, packs,
  trailing return types, template-ids in types, explicit instantiation,
  qualified declarator-ids, general sizeof-of-expression, typeid/noexcept)
  throw -> `EXIT_FAILURE`, which the failing fixtures expect; special
  member declarations and access labels are skipped without binding.

## Validation

- `make -C pa11 test` for the local suite while iterating, then root
  `make test-report-through-pa11` (the exit criterion) to prove PA1-PA10
  behavior is preserved (shared `sema/type.*` and AST parser changes are
  the regression surface: nsdecl/nsinit and the PA10 dump).
- `perl scripts/cppgm_file_audit.pl --stage pa11 --paths dev/src` for the
  architecture gate (file/function size, no shortcut smells).
