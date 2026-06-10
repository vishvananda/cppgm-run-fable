# PA7 (nsdecl) Plan

## Goal

`nsdecl` runs translation phases 1-7 over each srcfile and prints the
semantic model (namespaces, variables, functions, with canonical types)
in the handout format. Inputs are guaranteed to match `pa7.gram` and be
well-formed; anything else is undefined behaviour (the tool may exit
EXIT_FAILURE, never crash silently into wrong-but-plausible output).

## Pipeline

Reuse the existing per-srcfile pipeline unchanged: `Preprocessor` →
`PostTokenizer` → collected `vector<PostToken>`. The new semantic parser
consumes `PostToken` directly (it carries keyword/operator identity,
identifier spellings, and literal type + value bytes, which the array
bound rule needs); the PA6 `ParseToken`/`Parser` recognizer stays as-is
for `recog` and is not modified.

## New code (dev/src/sema/)

Semantic infrastructure lives in a new `sema/` subdirectory so later
assignments (PA8 nsinit onward) extend the same model.

- `sema/type.h` / `sema/type.cpp` — immutable `Type` nodes shared via
  `shared_ptr<const Type>`: fundamental (reusing `EFundamentalType` and
  the PA2 canonical names from `FundamentalTypeName`), pointer,
  lvalue/rvalue reference, array (known/unknown bound), function
  (parameter list + variadic flag). cv-qualification is two flags on a
  node; factories enforce the structural rules once:
  - cv applied to an array re-applies to the element type (8.3.4p1);
    cv applied to a reference is ignored (8.3.2p1).
  - reference-to-reference collapses (8.3.2p6): && of && is &&,
    everything else is &.
  - `AdjustParameterType` (8.3.5p5): array→pointer to element,
    function→pointer to function, strip top-level cv.
  - `PrintType` renders the recursive PA7 description format.
  - array-completion merge for redeclarations (`extern int a[]; int
    a[10];` keeps one entity whose type becomes `array of 10 int`).

- `sema/entity.h` / `sema/entity.cpp` — entity model + output walk.
  `Namespace` holds: identity (name / unnamed / inline / parent), the
  name→binding map, and three first-declaration-ordered print lists
  (variables, functions, member namespaces incl. the unnamed member).
  A `Binding` names one of: `Variable*`, `Function*`, `Namespace*`
  (member or alias — aliases bind the *same* `Namespace` object and are
  never in the print list), or a typedef (`TypePtr`). `Namespace` also
  records the using-directives that lexically appeared in it (explicit
  ones plus the implicit ones created by unnamed and inline member
  namespaces, 7.3.1.1p1 / 7.3.1p9). A `SemaModel` arena owns all
  entities; everything else uses raw non-owning pointers.
  `DescribeNamespace` walks the model in handout order (variables, then
  functions, then namespaces, each by first declaration).

- `sema/name_lookup.h` / `sema/name_lookup.cpp` — lookup per 3.4,
  parameterized by a filter (any entity / namespaces only):
  - Unqualified (3.4.1 + 7.3.4): for each enclosing namespace from
    innermost out, the visible set is its own bindings plus the
    bindings of every namespace nominated by an active using-directive
    whose anchor (nearest enclosing namespace of both the directive's
    container and the nominated namespace) is that scope. Directives
    are transitive: a directive found in a nominated namespace acts as
    if written at the original directive's container. Because the model
    is built incrementally during the single forward parse, "declared
    before this point" is automatic.
  - Qualified (3.4.3.2): S(X,m) — search X plus its inline namespace
    set; only if empty, recurse (with a visited set) into the
    namespaces nominated by using-directives in that set.

- `sema/decl_parser.h` / `sema/decl_parser.cpp` (split a
  `decl_parser_declarator.cpp` out if it grows past ~800 lines) —
  single-pass recursive-descent over pa7.gram with semantic actions;
  no backtracking over actions (the only positional lookahead is the
  token-scan that classifies `(` inside abstract-capable declarators).
  Key decisions, all driven by typed lookup state, not text:
  - declaration dispatch on the leading tokens (`;`, `inline`/
    `namespace` forms split by 1-2 token lookahead, `using` forms split
    by `namespace` / `ident =`, everything else simple-declaration).
  - decl-specifier-seq: keyword specifiers are consumed greedily with
    counters (sign, short, long count, base keyword, cv, typedef,
    storage class); an identifier or `::` in specifier position is a
    qualified/unqualified type-name iff no type has been seen yet —
    looked up immediately (must resolve to a typedef binding).
    Counter combination follows the 7.1.6.2 table.
  - declarators: parse syntax chunks (ptr-ops with cv, parenthesized
    sub-declarator, id-expression root, function/array suffixes), then
    compute the type: prefix ops apply left-to-right onto the base,
    suffixes apply right-to-left, then recurse into a parenthesized
    sub-declarator. Function suffixes evaluate their
    parameter-declarations to adjusted types inline; `(void)` becomes
    the empty list (8.3.5p4); array bounds take an integral non-UD
    literal whose value (via `LittleEndianValue`) must be > 0.
  - the `(` ambiguity inside parameter/abstract declarators
    (8.2p7-style): after `(`, `)` `...` or a decl-specifier first
    token means a parameters-and-qualifiers root; `*` `&` `&&` `(`
    means parenthesized declarator; an identifier/`::` is scanned
    ahead and classified by whether the (qualified) name resolves to a
    typedef. Named top-level declarators always take `(` as a
    parenthesized sub-declarator.
  - semantic actions: namespace definition find-or-create (reopen keeps
    first-declaration position; one unnamed member per parent; inline
    flag set at creation, OR-ed on reopen; unnamed/inline insert the
    implicit using-directive into the parent); namespace alias binds
    the target object under the new name; using-declaration binds the
    qualified-lookup result into the current namespace (entities stay
    owned by their first namespace, so they never reprint);
    using-directive appends to the current namespace's directive list;
    simple declarations bind typedefs or create/redeclare
    variables/functions. A qualified declarator-id only redeclares an
    existing member found by qualified lookup (test 250/260); an
    unqualified one matches only against the current namespace's own
    bindings. Redeclaration merges array completion into the existing
    entity (single overload entry is guaranteed by the handout).

- `dev/nsdecl.cpp` — tool entry mirroring `recog.cpp`: shared
  `PredefinedObjectMacros`, per-file fresh pipeline + `SemaModel`,
  writes the `<n> translation units` header and per-TU descriptions to
  the outfile. Any per-file error aborts with EXIT_FAILURE (errors are
  UB for PA7; tests never hit this path).

## Build wiring

`FRONTEND_OBJ_BASENAMES_nsdecl` = the preproc set + `sema/type`,
`sema/entity`, `sema/name_lookup`, `sema/decl_parser` (+ the optional
declarator split). No changes to other tools' sets.

## Validation

1. `make test-report ACTIVE_TEST_REPORT_PAS='pa7'` while iterating
   (all 24 local tests; there is no cppgm.tests/course/pa7 directory).
2. `make test-report-through-pa7` as the exit gate (pa1-pa6 must stay
   green; nsdecl links from shared sources, so rebuild fallout shows
   here).
3. `perl scripts/cppgm_file_audit.pl --stage pa7 --paths dev/src`.

Spot-check semantics beyond the fixtures with `nsdecl-ref` on
synthesized inputs (transitive using-directives, inline namespace
qualified lookup, reopened namespace ordering) before calling it done.
