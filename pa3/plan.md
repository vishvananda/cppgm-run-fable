# PA3 ctrlexpr — design plan

## Goal

`ctrlexpr` reads a C++ source file on stdin, applies translation phases 1-3
(reusing the PA1 pipeline unchanged), splits the preprocessing-token stream
into logical lines at `new-line` tokens, and evaluates each non-empty line as
a conditional-inclusion controlling expression (16.1). Output per line is a
decimal `intmax_t` value, a decimal `uintmax_t` value with `u` suffix, or
`error`; a final `eof` line ends the output. Phase 1-3 failures exit
EXIT_FAILURE; everything else is a per-line `error`.

## Architecture / ownership

- `dev/src/ctrl_expr.{h,cpp}` (new): all PA3-specific logic.
  - `CtrlExprStream : IPPTokenStream` — accumulates one logical line of
    converted tokens, evaluates at each `new-line`/eof, prints results and
    the trailing `eof`.
  - `EvaluateControllingExpression(tokens, is_defined) -> string` — exposed
    separately so the PA6 preprocessor can evaluate `#if`/`#elif` token
    sequences directly (post macro expansion) without the line splitter.
  - `IsDefinedFn` callback — PA3 wires the mock; later assignments wire the
    real macro table. Keeps `defined` ownership outside the evaluator.
- `dev/ctrlexpr.cpp` (replace stub): driver only — read stdin, run
  `TranslateSource` + `TokenizePPTokens` into a `CtrlExprStream`, catch
  translation/tokenization exceptions as EXIT_FAILURE. Hosts
  `PA3Mock_IsDefinedIdentifier` (first UTF-8 code unit odd => defined),
  which is assignment-local, not compiler state.
- `dev/frontend_source_sets.mk`: ctrlexpr links
  `pp_tokenizer source_translation utf8 lex_char_classes post_token
  numeric_literals text_literals ctrl_expr`. The PA2 `post_tokenizer`
  (string concatenation + keyword folding) is deliberately NOT reused: PA3
  is in `identifier_or_keyword` context.

## Token conversion (per pp-token, PA3 context)

Stored as `PostToken`s so PA2's typed literal analysis is reused verbatim:

- whitespace: discarded; new-line: line boundary.
- identifier: kept as PTK_IDENTIFIER — no keyword folding (PA1 identifier
  semantics; `auto` evaluates as 0, `true`/`false`/`defined` handled by the
  parser by spelling).
- pp-number: `AnalyzePPNumber` (typed integer/floating/UD analysis).
- character-literal / UD char literal: `AnalyzeCharLiteral`.
- pp-op-or-punc: `LookupSimpleTokenType`; the four preprocessing-only
  operators stay invalid. Alternative tokens (`and`, `not`, ...) arrive
  here from phase 3 and map to their OP_* types.
- string literals, UD strings, header-names, non-whitespace-chars: never an
  integral-literal; recorded so the line check rejects them. Per-token
  analysis exceptions are confined to the line (only phase 1-3 errors may
  kill the run).

Line pre-check (16.1/PA3): every token must be a non-array literal of
integral type, an identifier, or a grammar operator; otherwise `error`.
Integral types and their course-defined signedness:
signed = bool, wchar_t, char, signed char, short, int, long, long long;
unsigned = unsigned char/short/int/long/long long, char16_t, char32_t.
Literal value bytes (little-endian ABI representation from PA2) widen to 64
bits: sign-extend signed types, zero-extend unsigned.

## Parser

Hand-written predictive top-down parser over the converted token vector,
exactly the README grammar: conditional (`?:`, right associative) over a
10-level left-associative binary ladder (`||`, `&&`, `|`, `^`, `&`,
`==/!=`, `</>/<=/>=`, `<</>>`, `+/-`, `*//%`) over unary (`+ - ! ~`) over
primary (integral-literal, parenthesized expression, `defined id`,
`defined ( id )`, identifier). Left recursion is handled by iteration at
each binary level. `true`=>1, `false`=>0, any other plain identifier=>0,
`defined` queries the callback. Trailing tokens after a complete
expression, or any grammar mismatch => `error`.

## Static typing + evaluation (separate passes over the AST)

Per 16.2.4 all operands act as intmax_t/uintmax_t; each AST node carries a
statically computed signedness (260-cond-ret-type proves type propagates
from non-evaluated branches):

- literal: its own promoted signedness; identifier/defined/!/comparisons/
  `&&`/`||`: signed; unary `+ - ~`: operand's; arithmetic & bitwise
  binaries: unsigned if either side is (usual arithmetic conversions on
  the 64-bit types); shifts: left operand's type (070-double-shift proves
  `1 << 60u` stays signed); `?:`: UAC of the two branches.

Evaluation walks the tree post-order on 64-bit bit patterns (wrap
semantics; signed right shift sign-preserves, course-defined). Short
circuit: `&&`/`||` evaluate the right side only when needed; `?:` evaluates
only the chosen branch (250-eval-order). Course-defined evaluation errors
(only when the node is actually evaluated): division/modulus right operand
zero; signed INT64_MIN / -1 and % -1 (500-integer-overflow); shift count
negative (in the right operand's own type) or >= 64. Errors propagate to a
per-line `error`.

## Validation

- `make test-pa3` then root `make test-report-through-pa3` (exit
  criterion; must keep PA1/PA2 green — no shared-file behavior changes).
- `perl scripts/cppgm_file_audit.pl --stage pa3 --paths dev/src` clean.
