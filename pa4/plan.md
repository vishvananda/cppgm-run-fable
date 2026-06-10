# PA4 macro — design plan

## Goal

`macro` reads a C++ source file on stdin, applies phases 1-3 (PA1 pipeline
unchanged), splits the preprocessing-token stream into `#define`/`#undef`
directives and text-sequences, macro-replaces each text-sequence with the
macro table as of that point (16.3 with the course-defined nesting
semantics), and runs the replaced tokens through phases 5-7 (PA2
PostTokenizer, one instance for the whole file so phase-6 string
concatenation spans directive boundaries). Output format is PA2 posttoken;
any error exits EXIT_FAILURE (stdout is only compared on success).

## Architecture / ownership

- `dev/src/pp_token.{h,cpp}` (new): `PPToken` — the typed value form of one
  IPPTokenStream emission (kind + spelling) plus the state macro replacement
  attaches to tokens: `ws_before` (a whitespace-sequence preceded it),
  `blacklist` (blue paint: macro names unavailable to this token),
  `noninvokable` (sticky abort flag), `paste_op` (this `##` came from a
  replacement list and operates; substituted `##` tokens are inert).
  `PPTokenCollector : IPPTokenStream` folds whitespace runs into the next
  token's `ws_before`, keeps `new-line` as a real token (directive
  structure), and ends at eof. `EmitPPToken` replays a token into any
  IPPTokenStream (drives the PA2 PostTokenizer). Spelling helpers
  (`IsHash`, `IsHashHash`, lparen/rparen/comma/ellipsis) treat the digraph
  spellings `%:` / `%:%:` as `#` / `##`.
- `dev/src/macro_table.{h,cpp}` (new): definition-side semantics.
  `MacroDefinition` (object/function-like, variadic, parameter list,
  replacement list with validated `#`/`##` structure) and `MacroTable`
  (`Define`/`Undef` parse the directive token line after the
  `define`/`undef` keyword). Owns all definition-time errors: missing or
  non-identifier name, malformed identifier-list, duplicate parameters,
  `...` not last, function-like `#` not followed by a parameter, `##` at
  either end of the replacement list, `__VA_ARGS__` outside a variadic
  replacement list (as name, as parameter, in a non-variadic replacement,
  or in `#undef`), `#undef` trailing tokens, object-like must-whitespace
  before a non-empty replacement, and 16.3p1 redefinition identity
  (parameter spellings and replacement spelling/kind sequence with
  whitespace *presence* between tokens; leading/trailing whitespace
  trimmed).
- `dev/src/macro_expand.{h,cpp}` (new): `MacroExpander` — the rescan engine
  over a token deque. Invocation recognition (function-like names need a
  `(`, possibly produced by earlier replacements or later source tokens in
  the same text-sequence), argument collection (raw, paren-balanced, commas
  split at depth 0, trailing arguments fold into `__VA_ARGS__`), argument
  count checks (single empty argument is zero args for a zero-parameter
  macro; variadic requires more args than named parameters, C++11 16.3p4),
  substitution (`#` stringize of raw args, `##` paste of raw args with
  placemarkers, ordinary parameters take the recursively expanded
  argument), paste retokenization through the real phase 1-3 pipeline
  (must yield exactly one preprocessing-token), and the blue paint rules
  below. Bare `__VA_ARGS__` reaching a scan is an error.
- `dev/src/macro_preprocess.{h,cpp}` (new): file-level phase-4 walk.
  Recognizes directive lines (`start-of-file|new-line` + optional
  whitespace + `#`), dispatches define/undef to the table, rejects other
  directives (out of contract for PA4), batches maximal text-sequences,
  feeds expansion output into a caller-supplied IPPTokenStream, and emits
  the final eof. PA5 extends this walk with the remaining directives.
- `dev/macro.cpp` (driver): stdin -> TranslateSource -> TokenizePPTokens
  -> PPTokenCollector -> MacroPreprocessor -> PostTokenizer -> stdout,
  exceptions to EXIT_FAILURE.
- `dev/frontend_source_sets.mk`: macro = posttoken set + `pp_token
  macro_table macro_expand macro_preprocess`.

## Blue paint (course nesting semantics, pinned by fixtures)

Per-token `blacklist` set; `noninvokable` aborts an invocation permanently.
On invoking macro M with head token T (and closing paren token R for
function-like invocations):

- replacement-origin tokens (copies from M's replacement list, stringize
  results, paste results): blacklist := base, where
  base = (T.blacklist ∩ R.blacklist) ∪ {M} for function-like and
  base = T.blacklist ∪ {M} for object-like.
- argument-origin tokens (substituted raw or expanded argument tokens):
  blacklist := own ∪ T.blacklist ∪ {M} — the full head paint, not the
  intersected base, and paint acquired during argument expansion is kept.

The intersection with R reproduces standard rescan behavior when an
invocation closes with cleaner tokens than its head (course tests
600-pasted-helper-macro-rescans, 600-tail-helper-macro-rescans; the
Boost.PP CAT/FILLER idioms). The argument rule is the course deviation
from C: pa4/tests 910-recurse2 (`g(f)(g)(3)` -> `2 1 g(3)`) and the
`t(t(g)(0) + t)(1)` line of 700-redef-q require argument-origin tokens to
inherit the head's full paint. 900-recurse, 600/650-recurse, and the
README f(z) trace hold under both and stay correct.

A head whose own name is in its own blacklist is flagged noninvokable and
emitted as a plain identifier (the `(` and arguments are left in the
stream). Since blacklists never shrink, the flag is equivalent to
re-checking, but it documents the course rule that a skipped name is never
reconsidered.

## Substitution details

- Whitespace: `ws_before` only matters for directive identity and
  stringize. A substituted argument's first token takes the parameter
  slot's `ws_before`; later tokens keep their own. A pasted token takes
  the left operand's. New-lines inside a text-sequence act as whitespace.
- Stringize (16.3.2): raw argument spellings, single space where
  `ws_before` is set between tokens, `\` before `"`/`\` inside string and
  character literal spellings only; result is a real string-literal
  pp-token re-entering the stream (it may concatenate in phase 6).
- Paste (16.3.3): left-to-right; operands are the last/first tokens of the
  adjacent raw-argument item (other argument tokens substitute
  unchanged); empty arguments are placemarkers (pm ## t -> t,
  t ## pm -> t, pm ## pm -> pm); spellings are concatenated and
  retokenized via TranslateSource + TokenizePPTokens (raw-string
  reversion keeps trigraph-looking content intact, course test 410);
  anything but exactly one resulting pp-token is an error; the result is
  rescanned and may invoke (700-strlit-q HIGHLOW, 552 makes a
  user-defined raw string literal).
- Only `#`/`##` tokens written in the replacement list operate;
  substituted ones are inert pp-tokens that posttoken reports as
  `invalid` (course tests 150-hash-outside, 185-multiple-pp-tokents).
- Arguments are parsed from raw tokens before any expansion, so commas
  produced by expansion never split arguments (700-redef-q `m(f)` with
  `w == 0,1`).

## Deliberate reference divergences (standard over ref parity)

Probing `macro-ref` on non-fixture inputs shows it accepts two argument
count violations gcc also tolerates: excess arguments to a non-variadic
macro fold into the last parameter (`#define f(x,y) x y` + `f(1,2,3)`
gives `1 2 , 3`), and a variadic invocation may omit the variadic part
entirely (`#define A(x,...) x` + `A(1)`). C++11 16.3p4 makes both
ill-formed ("shall equal" / "there shall be more arguments"), no checked-in
fixture in pa4 or pa5 exercises the lenient form, and
TESTING_AND_REFERENCES.md prefers the handout and standard over reference
parity off-fixture, so this implementation reports EXIT_FAILURE for both.
All other probed edges (null directive, unknown directive, digraph `%:`
directives, `#` spacing, unterminated invocations, empty object-like
macros, redefinition whitespace identity, `__VA_ARGS__` pastes) match the
reference exactly.

## Validation plan

- `make -C pa4 test` against pa4/tests and cppgm.tests/course/pa4 (the
  fixtures above are the oracle for every semantics decision).
- `make test-report-through-pa4` as the exit criterion; PA1-PA3 reuse the
  same pipeline files, so any tokenizer-level change would surface there
  (none are planned: PA4 is purely additive).
- `perl scripts/cppgm_file_audit.pl --stage pa4 --paths dev/src`.
