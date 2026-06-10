# PA6 recog — design plan

## Goal

`recog -o <outfile> <srcfile1> ... <srcfileN>` runs the PA5 pipeline
(translation phases 1-6 plus the tokenization part of phase 7) over each
source file, converts the resulting token sequence into PA6 parse
tokens, and recognizes it against the `pa6.gram` `translation-unit`
grammar. The outfile gets `recog <N>` followed by one `<srcfile> OK` or
`<srcfile> BAD` line per file; any per-file error (open failure, PA5
error, phase-7 invalid token, parse failure) yields `BAD` for that file
only, and the tool exits EXIT_SUCCESS. A parse tree is printed to
stdout on success (free-form debugging output; only the outfile and
exit status are gated).

## Architecture / ownership

The parser is the first piece of the real compiler front end after the
preprocessor, so it lives in its own subdirectory `dev/src/parse/` and
consumes the PA5 `Preprocessor` + PA2 `PostTokenizer` output unchanged.

- `dev/src/predefined_macros.{h,cpp}` (new, shared): the
  course-defined predefined object macros (and the one-shot
  asctime-derived `__DATE__`/`__TIME__` capture) move out of
  `dev/preproc.cpp` so `preproc`, `recog`, and every later tool that
  embeds the preprocessor share one definition.
- `dev/src/parse/parse_token.{h,cpp}` (new): `ParseToken` — the PA6
  terminal vocabulary. Kinds: simple (an `ETokenType` from PA2),
  identifier, literal, ST_RSHIFT_1, ST_RSHIFT_2, ST_EOF. Carries the
  source spelling (for mock name lookup, `override`/`final`,
  `ST_EMPTYSTR` = literal spelled `""`, `ST_ZERO` = literal spelled
  `0`). `BuildParseTokens` converts a collected `vector<PostToken>`:
  PTK_SIMPLE OP_RSHIFT becomes the ST_RSHIFT_1 ST_RSHIFT_2 pair, all
  literal kinds (including user-defined) become TT_LITERAL, PTK_EOF
  becomes ST_EOF, and PTK_INVALID throws (the file is BAD; pinned by
  course/pa6/invalid-token-balanced-scan-bad).
- `dev/src/parse/parse_tree.{h,cpp}` (new): `ParseNode` — the handout's
  placeholder dynamic AST: a nonterminal tag (static name string) plus
  ordered children that are sub-nodes or token leaves. One printer
  renders the indented tree to stdout. The tree only needs to witness
  the recognized structure; it is replaced by typed AST classes in
  later assignments.
- `dev/src/parse/parser.h` (new): class `Parser` — token vector,
  lookahead index, bracket-context stack, and one `Parse<Nonterminal>`
  member per grammar nonterminal. Parse functions return a
  `ParseNodePtr` (null = failure, lookahead restored to entry — the
  handout's interface discipline).
- Parse function bodies are split by grammar area (the A.3-A.13
  clustering, one concern per file):
  - `parser_core.cpp`: token/bracket/save-restore infrastructure,
    `translation-unit`, mock name predicates (identifier contains
    `C`/`T`/`Y`/`E`/`N`).
  - `parse_names.cpp`: names and templates — *-name, simple-template-id,
    nested-name-specifier, id-expression, operator/conversion/literal
    operator ids, template-ids/arguments, decltype/typename specifiers.
  - `parse_expr.cpp`: expressions — primary, lambda, postfix, unary,
    new/delete, cast, the binary-operator ladder, conditional,
    assignment, throw, expression-list.
  - `parse_stmt.cpp`: statements — labeled/expression/compound,
    selection, iteration, jump, condition, try/handler.
  - `parse_decl.cpp`: declarations — declaration dispatch, simple/block
    declarations, decl-specifiers, enums, namespaces, using, linkage,
    static_assert, template/explicit declarations, attributes.
  - `parse_declarator.cpp`: declarators — (abstract) declarators,
    ptr-operators, parameters, type-id, initializers, function bodies.
  - `parse_class.cpp`: classes — class heads/keys, members, bitfields,
    bases, mem-initializers, virt-specifiers.
- `dev/recog.cpp` (rewritten tool entry): argument handling identical
  to `preproc`, per-srcfile pipeline = collecting `IPostTokenStream`
  -> `PostTokenizer` -> `Preprocessor::ProcessSourceFile`, then
  `BuildParseTokens` + `Parser::ParseTranslationUnit`. Catches per-file
  `std::exception` as BAD; tool-level errors (usage, outfile) exit
  EXIT_FAILURE. Keeps the starter mock-lookup semantics via the parser's
  predicates.
- `dev/frontend_source_sets.mk`: `recog` gets the full preproc set plus
  `predefined_macros` and the `parse/*` objects.

## Parsing strategy

Recursive descent with backtracking, exactly the handout's interface
discipline: every parse function either returns a node with the
lookahead one past its parse, or null with the lookahead restored.
Sequences (`x*`, `x+`, `(OP_COMMA x)*`) are parsed greedily; each list
iteration snapshots the position so a trailing separator backs out
cleanly (`{1,2,}`, parameter lists before `, ...`).

Alternatives that share a prefix are ordered longest-first
(template-id before plain identifier in unqualified-id,
`operator new []` before `operator new`, array-delete before delete).
Where the grammar is genuinely ambiguous, the choice is pinned to the
standard's disambiguation rules:

- 6.8 statement/declaration: `statement` tries `declaration-statement`
  before `expression-statement`; both end at `;`, so ordered choice
  with backtracking implements "anything that can be a declaration is
  a declaration" (`C(a);` declaration vs `C(a)->m = 7;` expression).
  `for-init-statement` orders the same way.
- 8.2 function/object declaration: `(` after a declarator-id tries
  `parameters-and-qualifiers` before falling back to a parenthesized
  initializer (`C x(int());` function vs `C y((int)a);` object).
- 8.2/7 parameter type vs redundant parens: `parameter-declaration`
  tries the abstract-declarator (type) reading before the named
  declarator reading, validated against FOLLOW = {`,` `)` `...` `=`}
  so `int(C)` is a function type while `int(a)` declares parameter `a`
  and `int *x` falls through to the named reading.
- 8.2/3 type-id vs expression: `template-argument`, `sizeof(...)`,
  `typeid(...)`, and `alignas(...)` try the type-id alternative first.
  `template-argument` validates FOLLOW = {`,` `...` close-angle} so
  `TC1<C+1>` backs out of the type reading and reparses as a
  constant-expression.
- `member-declarator` validates FOLLOW = {`,` `;`} so `char b : 0?1:2;`
  backs out of the declarator reading and parses as a named bitfield.
- `condition` tries `condition-declaration` before `expression` (6.4).
- `assignment-expression` is factored: parse the binary ladder once,
  then extend with `? :` (conditional) or an assignment-operator +
  initializer-clause, avoiding the triple reparse of the grammar's
  alternatives.

Nullable-loop hazard: the only nullable starred element reached by a
separator loop is `attribute-part` inside `attribute-list` (`[[,]]`),
which terminates because the comma is consumed; every `x*`/`x+` loop
element consumes at least one token.

## decl-specifier-seq type-name rule

The handout rule — a type-name is part of a `decl-specifier-seq` iff no
previous type-specifier other than a cv-qualifier was seen — is
implemented as state in the shared specifier-sequence parser: keyword
type-specifiers (`int`, `unsigned`, ...), class/enum specifiers,
elaborated-type-specifiers, decltype, and typename-specifiers set
`seen_type`; cv-qualifiers and non-type decl-specifiers do not; once
`seen_type` is set the identifier-led `nested-name-specifier? type-name`
(and `... template simple-template-id`) forms are no longer attempted,
so `C1 C2;` declares `C2` of type `C1`. The same helper drives
`type-specifier-seq` and `trailing-type-specifier-seq` (with the
decl-only specifiers disabled) so `new C1 C2` and friends resolve the
same way.

## close-angle-bracket (14.2.3)

The parser keeps a bracket-context stack. Consuming `(`/`[`/`{` pushes,
consuming the matching closer pops (the grammar opens and closes these
within one alternative, so push/pop balance within every parse
function). Entering a template-argument/parameter list or a
`cast< type-id >` consumes `OP_LT` and pushes an angle context;
`close-angle-bracket` accepts OP_GT, ST_RSHIFT_1, or ST_RSHIFT_2 and
pops it. Backtracking restores the stack by truncating to the saved
depth, which is sound because no parse function pops below its entry
depth.

The 14.2.3 refusals: `relational-operator OP_GT` and `shift-operator
ST_RSHIFT_1 ST_RSHIFT_2` fail when the innermost bracket context is an
angle, so the first same-level closing token always closes the bracket
pair (`TC1< 1>2 >` BAD) while nested non-angle contexts re-enable the
operators (`TC1<(1>2)>`, `TC1<TC2<(6>>1)>>` OK). A split `>>` closes
two levels: the inner list consumes ST_RSHIFT_1, the outer ST_RSHIFT_2
(`TC1<TC2<1>>` OK, `TC1<TC2<6>>1>>` BAD because the dangling `1` cannot
start a declarator).

## Mock name lookup

Identifier category = spelling probe (contains `C`/`T`/`Y`/`E`/`N` for
class/template/typedef/enum/namespace-name). The gates live exactly
where the grammar names a `*-name` nonterminal: `template-name`
requires `T` before `<` opens a simple-template-id;
`nested-name-specifier-root` requires a type-name or namespace-name
before `::` (suffixes accept any identifier);
`class-name`/`enum-name`/`typedef-name` gate their identifier
alternatives, while their simple-template-id alternatives follow the
grammar literally (no extra letter requirement — `type-name` lists
simple-template-id unconditioned). `ST_OVERRIDE`/`ST_FINAL` are
spelling checks on identifier tokens in virt-specifier positions only.

## Validation

- `make -C pa6 test` for tests/ and course/pa6 (32 + 11 cases: the
  expression/statement/declaration ladder, ambiguity pins from 6.8 and
  8.2, close-angle-bracket OK/BAD triples, context-sensitive keywords,
  bitfields, try/catch shapes).
- `make test-report-through-pa6` as the exit criterion (PA1-PA5 stay
  untouched except the predefined-macro move in `preproc`, which is a
  pure code motion).
- `perl scripts/cppgm_file_audit.pl --stage pa6 --paths dev/src` for
  file/function-size and structure budgets — the per-area parser split
  above keeps every file well under the limits with cohesive
  boundaries.
- `recog-ref` probes for grey-zone calls (ill-formed but syntactically
  valid constructs) if a suite case disagrees.

## Post-implementation audit

Differential probing against `recog-ref` (targeted construct probes
plus a 250-file randomized sweep over declaration/statement/expression
templates with mock-typed identifiers) found and fixed three real
parser gaps, all confirmed by the suites staying at 43/43:

- `abstract-pack-declarator` must be tried before the bare
  `ptr-operator+` reading: `(const CMixins&... mixins)` otherwise
  commits the abstract `&` reading (OP_DOTS satisfies the parameter
  FOLLOW set) and orphans the parameter name (tests/400-dots).
- `sizeof` tries the unary-expression reading before the parenthesized
  type-id: the expression reading can consume postfix suffixes beyond
  the parenthesized group (`sizeof(C1)(x)` is a call of a
  parenthesized id-expression), which the grammar reduces and the
  reference accepts; type-only operands still fall through.
- The identifier-led simple-type-specifier retries without its
  nested-name-specifier when the greedy root consumes the type-name
  itself: `Cfoo::~Cfoo() {}` reduces as decl-specifier `Cfoo` plus
  declarator `::~Cfoo()` (a global-:: qualified-id), which the
  one-shot greedy parse missed.

Robustness fixes from adversarial inputs: each srcfile's pipeline runs
on a worker thread with a 512MB stack (recursion depth is proportional
to input nesting; 100k-deep parens now parse in ~2s where the default
8MB stack overflowed at ~20k), and the stdout tree dump clamps its
indentation (unbounded indent made the dump quadratic in nesting
depth). Throughput on 10-20k-line files is linear at roughly 5x the
reference's wall clock, consistent with the project's PA1-PA5 constant
factor.

Known reference divergences, deliberately not matched: probing shows
the published `recog-ref` is a fuller, later front end rather than the
documented PA6 contract — it accepts the handout's own "wont match
PA6" example (`C(); ~C(); operator bool();` members), applies no mock
name-lookup gating (`x y;`, `class x {};`, `a::b * c;`, `x<3> y;` all
OK), accepts statements at translation-unit scope (`return 1;`,
`case 1: ;`), recovers from junk ending in a semicolon (`x + ;`,
`= ;`, even `TC1< 1>2 > x1;` at TU scope — the exact construct its own
checked-in fixture marks BAD inside a function), accepts C++-later
constructs (`int x : 3 = 1;`, `for (auto& [x] : ys)`), and reports
nonexistent srcfiles OK. Per the repo rules (checked-in fixtures are
the only gate; prefer the handout and the standard over reference
parity), this implementation keeps the handout's mock gating, the
pa6.gram productions, and BAD for unopenable files; every fuzz
divergence is the reference accepting outside the documented grammar,
with zero cases of this parser accepting what the grammar rejects.

The remaining audit warning ([bad-division] on parser.h) is a
heuristic false positive: the header holds one-line method
declarations (one parse function per nonterminal on one Parser class,
the handout's recommended architecture) and no implementation bodies;
the line counter treats any `;`-bearing line as body weight. Splitting
the class declaration across headers would be a mechanical split, so
the single declaration header stays.
