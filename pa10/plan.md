# PA10 Plan: `cppgm++ --emit-ast`

## Goal

Replace the PA6 recognizer boundary with a tree-building parser for the
shared `cppgm++` source grammar (`pa10.gram` -> `shared/source.gram`) and a
deterministic AST text dump gated by the checked-in `.ref` fixtures.

## Pipeline

`run_emit_ast_mode` in `dev/cppgm++.cpp`:

1. For each srcfile, run translation phases 1-7 with the existing PA5
   pipeline (`Preprocessor` + `PostTokenizer`, predefined macros), exactly as
   `recog.cpp` does.
2. Convert phase-7 tokens with the existing `BuildParseTokens`
   (`dev/src/parse/parse_token.h`): OP_RSHIFT splits into ST_RSHIFT_1/2,
   one terminal per literal, ST_EOF last.
3. Parse with the new `AstParser` (`dev/src/ast/`), producing a typed
   `TranslationUnitDecl`.
4. Print all units to the outfile with the deterministic printer:
   `<n> translation units`, then `start translation unit <k>` /
   `translation-unit` dump / `end translation unit` per unit.

Any pipeline, tokenization, or parse error exits `EXIT_FAILURE`. Parsing
runs on a large-stack worker thread (same rationale as `recog.cpp`).

## Ownership boundaries

- `dev/src/ast/ast.h` (+`ast.cpp`): typed AST node model, owned via
  `unique_ptr`, no parser or printer knowledge. PA11/PA12 consume this.
  - Names are structural (`AstName` = optional global `::`, parts with
    identifier/template-arguments/destructor/operator/conversion/decltype
    forms, `typename`/`template` keyword flags), never stored as source
    text spans.
  - `AstDeclarator` is the flat, source-ordered item list the dump shows:
    ptr-operator (incl. member pointers), cv-qualifier, parameter-pack,
    declarator-id, nested declarator, parameter clause, function
    qualifiers, trailing return type, array suffix.
  - Declarations, statements, expressions, type-ids, initializers are
    kind-tagged structs with typed fields.
- `dev/src/ast/ast_text.*`: flattening of structured names, type-ids and
  expressions into the single-line annotation spellings the dump format
  uses (`Box<T>::~Box`, `const P&`, `decltype(f<T>())::value`). Rendering
  only; never parsed back.
- `dev/src/ast/ast_printer.*`: indentation-based deterministic dump in the
  fixture vocabulary.
- `dev/src/ast/ast_parser.h` + `ast_parser_core.cpp` and
  `ast_parse_{names,expr,stmt,decl,declarator,class}.cpp`: recursive
  descent with save/restore backtracking over the PA6 terminal sequence.
- The PA6 recognizer (`dev/src/parse/parser.*`) stays as-is for `recog`;
  PA10 reuses only its terminal preparation (`parse/parse_token.*`).

## Parsing approach

Ordered-choice recursive descent (the PA6 discipline: a parse function
either returns a node with the lookahead one past its parse, or restores
the entry state). The 14.2.3 close-angle handling follows the PA6 parser:
a bracket stack where `(`/`[`/`{` push, template argument/parameter lists
push an angle context, and OP_GT / ST_RSHIFT_1 / ST_RSHIFT_2 refuse to act
as operators while the innermost open bracket is an angle.

Template-argument-clause attempt outcomes are memoized by `<` token
position (`clause_memo_`), invalidated whenever the name table or scope
stack changes — the only context a clause parse depends on besides the
tokens. Uncommitted attempts (expression-context `name<` without a
following `(`, qualifier template-ids without a following `::`) consult
the memo instead of re-parsing; without this, nested template-argument
ambiguities re-parse the same span once per enclosing alternative,
which compounds exponentially (`a<a<...<1+0>+0>+0>` at depth 12 ran
past 10s; with the memo every depth rejects instantly, matching the
reference).

Disambiguation rules (validated against the fixtures):

- block-item / for-init / class-member: declaration before expression
  statement (6.8).
- condition: `decl-specifier-seq declarator initializer` before
  expression; the declaration form requires the initializer, so
  `if (int x)` fails (`200-bad-condition-declaration`).
- parameter-declaration: abstract (type-id) reading before the named
  reading (8.2/7).
- template-argument: type-id before assignment-expression, committed only
  at `,` or close-angle.
- class members: access label, bit-field, declaration, then
  special-member declaration/definition; TU declarations follow the
  grammar ordering with special-member-definition before
  function-definition before simple-declaration.

## Syntactic name table

PA10 must decide "is this identifier a type-name / template-name" without
semantics. The parser keeps a scope stack (TU, namespace body, class body,
function/lambda body, compound statement, template parameter scope,
for/handler scopes) mapping identifier -> {Type, Template, Value} flags,
with an undo log so backtracking rolls registrations back.

- Registers: typedef/alias names and class/enum names as Type; enumerator,
  variable, function and parameter names as Value; template-declarations
  re-register the declared name with Template (class/alias templates also
  Type); type template-parameters as Type, non-type as Value,
  template-template as Type+Template. Members register in the class scope
  only; using-directives/declarations and namespace names register
  nothing (fixtures `200-*-imported-template-id-type` rely on the
  imported names staying unknown).
- Lookup is nearest-scope-first, so a local variable shadows an outer
  typedef (`300-declaration-statement-ambiguity`) and a local typedef
  shadows an outer function (`300-local-typedef-shadows-...`).
- Type positions accept: declared Type names, and undeclared identifiers
  (optimistic), but never names whose nearest declaration is Value-only.
- `name <` starts a simple-template-id attempt when the name's nearest
  declaration is a Template; for unknown names the attempt is kept only
  in type contexts, or in expression contexts when the parsed template-id
  is immediately followed by `(` (`200-member-relational-*`,
  `200-qualified-member-comparison-template-arg`,
  `200-switch-case-template-id-call` pin this rule). Inside a
  nested-name-specifier a template-id is committed only when `::`
  follows.

## Dump format

The fixtures define the format; the full vocabulary was extracted from all
134 refs. Key conventions:

- Two-space indentation per level; annotated nodes append one space and
  the annotation.
- Token-style annotations are `TOKENTYPE:spelling` (`decl-specifier
  KW_INT:int`, `cast-expression OP_LPAREN:` with empty spelling for
  C-style casts, `member-expression OP_DOT:.`).
- Name-style annotations are flattened structure: qualified parts joined
  with `::`, template arguments joined with `,` and no spaces, type-id
  arguments as specifier words joined with spaces plus declarator tokens
  (`const From*`), expressions with no inserted whitespace and
  parentheses preserved, `typename `/`template ` keywords preserved
  inside nested arguments but `typename` dropped from a top-level
  specifier. Unqualified conversion ids render as `operator<type>`
  with no space; qualified ones as `NNS::operator <type>`.
- decl-specifier-seq children print as `decl-specifier` leaves (keyword
  token form, `TT_IDENTIFIER:name` for plain identifiers, flattened text
  otherwise), or as nested `class-specifier`/`enum-specifier`/
  `class-forward-declaration` nodes; type-specifier-seq uses
  `type-specifier`/`cv-qualifier`/`type-name`/`decltype-specifier`.
- The few reference quirks are reproduced and documented in the printer:
  `specifier explicit` prints bare while other member specifiers print as
  token annotations, and a declarator-less non-type template parameter
  with keyword-only specifiers prints its default literal as a token leaf
  (`literal TT_LITERAL:0`).

## Accepted syntax beyond `source.gram`

The fixtures require these extensions, implemented as real grammar
productions (not test-shape probes):

- linkage-specification: `extern <string-literal> { declaration* }` and
  the single-declaration form, dumped as `linkage-specification <lang>`.
- non-type template parameters (`source.gram` derives template-parameter
  from type-parameter only; `200-non-type-template-parameters` requires
  the declarator form).
- dynamic exception specifications `throw ( type-id-list? )` as a
  function qualifier.
- member-pointer ptr-operators `NNS::*` with cv-qualifiers.
- parameter packs in declarators (`Args&&... args`), declarator-less
  non-type template parameters, and `...` parameter clauses.
- GNU `__attribute__((...))` and `alignas(...)` after a class-key or
  among (member) specifiers, and `[[...]]` after a declarator, are parsed
  and discarded (balanced-token skip), matching the refs which omit them.

## Validation

1. `make -C pa10 test` / root `make test-report ACTIVE_TEST_REPORT_PAS='pa10'`
   for the 134 local fixtures (128 success dumps + 6 failure statuses)
   plus the `cppgm.tests/course/pa10` extension fixtures (lambda capture
   forms, ref pinned from the reference binary).
2. Root `make test-report-through-pa10` to prove PA1-PA9 stay green
   (recog and the rest of the pipeline are untouched, but cppgm++ build
   wiring changes).
3. `perl scripts/cppgm_file_audit.pl --stage pa10 --paths dev/src` for
   size/structure gates (sources <= 1500 lines, functions <= 120 lines).

## Architecture Review

The implementation matches the pipeline above; the audit (see
`audit.md`) confirmed each stage's ownership against the code:

- The driver (`dev/cppgm++.cpp`) reuses the unmodified PA5 pipeline and
  PA6 terminal preparation per srcfile; AST knowledge starts at
  `dev/src/ast/`. There are no test-shape or test-name gates and no
  fallback success paths: any pipeline or parse throw maps to
  EXIT_FAILURE in `main`, and `ParseTranslationUnit` requires ST_EOF.
- The dump is a rendering of the typed tree, never embedded text: every
  printer line walks `AstDecl`/`AstStmt`/`AstExpr` structure, and
  `ast_text` flattens that same structure for one-line annotations.
  Nothing parses rendered text back.
- Syntactic facts are typed, not stringly. Names are part lists with
  template arguments, operator texts, conversion type-ids, and decltype
  expressions; declarators are flat typed item lists; lambda captures
  are a capture-default token plus `{this, id, &id}` capture entries
  with pack flags (`AstLambdaCapture`), so PA11/PA12 never reparse
  `[&x,=]` spellings. The only stored source spellings are leaf token
  spellings (literals, keywords, identifiers), which is what the dump
  format requires.
- The syntactic name table is the parser's own concern: it feeds only
  the standard's syntactic disambiguations, registrations are
  undo-logged so backtracking is exact, and nothing downstream consumes
  it. PA11's semantic lookup starts from the AST, not from this table.
- Backtracking cost is bounded: save/restore is O(1) plus the undo tail,
  and the clause memo (above) removes the one exponential re-parse
  family. A 22k-line mixed input parses in ~0.7s; adversarial
  template-argument nesting rejects instantly at any depth.

## Final Architecture Review

Post-audit state: 519/519 through PA10 (134 local + 1 course fixture),
file audit clean (the one warning is the pre-existing PA6
`parse/parser.h` header-implementation division, untouched by PA10).
Remaining accepted trade-offs, none of which hide work from later
assignments:

- The parser accepts a small superset of `pa10.gram` on
  undefined-behavior inputs (e.g. expression-first `alignof(expr)`,
  lenient trait operands) where the references do the same; all
  fixture-pinned rejections still reject.
- `sizeof...(pack)` is not parsed: `source.gram` has no production for
  it, and the shared grammar is the authority over reference leniency.
- The persistent namespace/class child tables over-approximate scope
  contents across failed parses (children created during an abandoned
  attempt persist); this only widens the optimistic-unknown class of
  names, which the disambiguation rules already treat permissively.
- `Restore` truncates the bracket stack but cannot re-push entries
  popped below the save depth; this is safe because every parse
  function only consumes closers for brackets it opened itself (each
  alternative saves at its construct's start), verified across all
  Save/Restore sites during the audit.
