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
  - `CtrlExprCalculator` — the streaming evaluator (mirrors the reference
    binary's `Calculator`): converted tokens accumulate into an evaluation
    unit, `defined` operators fold as tokens arrive, each new-line emits at
    most one output line. Error state is deliberately NOT line-local (see
    the pinned reference semantics below).
  - `CtrlExprStream : IPPTokenStream` — adapts the PA1 emit channels:
    converts each preprocessing-token (PA2 literal analysis, no keyword
    folding) and feeds the calculator with its emit-channel provenance,
    which the analyzed token alone cannot carry.
  - `EvaluateControllingExpression(tokens, is_defined) -> string` — drives
    the same calculator over a single already-converted token sequence, so
    the PA6 preprocessor can evaluate `#if`/`#elif` lines directly.
  - `IsDefinedFn` callback — PA3 wires the mock; later assignments wire the
    real macro table. Keeps `defined` ownership outside the evaluator.
- `dev/ctrlexpr.cpp` (driver only): read stdin, run `TranslateSource` +
  `TokenizePPTokens` into a `CtrlExprStream`, catch translation/tokenization
  exceptions as EXIT_FAILURE. Hosts `PA3Mock_IsDefinedIdentifier` (first
  UTF-8 code unit odd => defined), which is assignment-local, not compiler
  state.
- `dev/frontend_source_sets.mk`: ctrlexpr links `pp_tokenizer
  source_translation utf8 lex_char_classes post_token numeric_literals
  text_literals ctrl_expr`. The PA2 `post_tokenizer` (string concatenation +
  keyword folding) is deliberately NOT reused: PA3 is in
  `identifier_or_keyword` context.

## Token conversion (per pp-token, PA3 context)

Stored as `PostToken`s so PA2's typed literal analysis is reused verbatim:

- whitespace: discarded; new-line: line boundary.
- identifier: kept as PTK_IDENTIFIER — no keyword folding (PA1 identifier
  semantics; `auto` evaluates as 0, `true`/`false` resolved by the parser,
  `defined` folded by the calculator).
- pp-number: `AnalyzePPNumber` (typed integer/floating/UD analysis,
  including the reference's GNU 0b binary-literal extension).
- character-literal / UD char literal: `AnalyzeCharLiteral`.
- pp-op-or-punc: `LookupSimpleTokenType`; `#`, `##`, `%:`, `%:%:` stay
  invalid. Alternative tokens (`and`, `not`, ...) arrive here from phase 3
  and map to their OP_* types; `new`/`delete` map to keyword tokens, which
  are outside the controlling-expression grammar.
- string literals, UD strings, header-names, non-whitespace-chars: never an
  integral-literal; they poison the line (only phase 1-3 errors may kill
  the run).

## Pinned reference semantics (line handling)

The README describes per-line evaluation with `error` for any invalid
line. The reference binary implements something considerably more
stateful; the grader compares against the reference, so the calculator
replicates it (each rule pinned by probes and differential fuzzing):

- Line check: every token of a line must be an identifier, a non-array
  integral literal, or a grammar operator — even AFTER a complete
  expression (`5 ,` is an error while `5 (` evaluates to 5).
- Poison (skip): a non-evaluable token — floating or UD literal, string,
  invalid character literal, non-grammar or invalid operator, stray
  character — makes the line print `error`; the unit restarts.
- Invalid integers: a pp-number rejected by the integer parser deferes the
  `error` to the line's new-line, but the unit (tokens before AND after
  it) survives and carries into the NEXT line's evaluation
  (`5uu 7` then `5` evaluates `7 5` -> 7). Float-shaped spellings (a dot,
  or an exponent marker e/E — p/P once an x switches to hex — directly
  followed by digit or sign) poison like real floats instead.
- A poisoned line that also defers an error keeps its poison for the next
  line (`5uu 3.5` then `5` -> error, error).
- Token-less lines print nothing and pending state passes over them.
- `defined` folds inline: `defined id` / `defined ( id )` -> signed 1/0.
  A non-folding token at the operand or `)` position defers an error and
  is consumed unanalyzed while the construct keeps waiting (`defined (
  3.5 a ) 6` folds `a` and evaluates `1 6`); a construct still open at
  the new-line defers an error and the folded unit carries over;
  `defined` opens even on poisoned lines.
- Partial line at end of input (only possible via the \UFFFFFFFF marker
  or a carried unit): discarded, never evaluated.

## Parser (per evaluation unit)

Hand-written predictive top-down parser, the README grammar: conditional
(`?:`, right associative) over a 10-level left-associative binary ladder
over unary (`+ - ! ~`) over primary. Left recursion by iteration.
Reference-pinned deviations:

- A grammar operator that cannot start a primary-expression is consumed
  as a recovery zero; parsing continues normally after it.
- Tokens left over after a complete top-level expression are ignored
  (`'a' int` -> 97); but a mismatched `)` or `:` inside a construct is a
  hard error (`(2 + == 4)` -> error), as is running out of tokens
  mid-parse (`1 +` -> error).

## Typing + evaluation (single pass over the AST)

Per 16.2.4 all operands act as intmax_t/uintmax_t on 64-bit bit patterns
(wrap semantics; signed right shift sign-preserves). The reference
computes signedness in the same pass as the value, so `Evaluate` returns
{value, is_unsigned, recovery} per node under an `enabled` flag:

- literal: its own promoted signedness; identifier/defined: signed.
- unary `+ - ~ !` keep the operand's signedness (reference-pinned: `!0u`
  is `1u`).
- arithmetic/bitwise binaries: usual arithmetic conversions, except a
  right operand with a pending recovery restores the left operand's
  signedness; shifts keep the left operand's type (070-double-shift).
- comparisons and `&&`/`||` yield signed int; comparison semantics treat
  an operand with a pending recovery as signed (`% - 9 > 0` compares
  signed); both absorb pending recovery events.
- `?:` walks both branches so signedness propagates from the branch
  evaluation does not select (260-cond-ret-type) while its evaluation
  errors and recovery zeros stay inert; value follows the chosen branch.
- recovery zero: value 0, unsigned only where evaluation is enabled, and
  carries a "recovery event" upward until a binary right-operand,
  comparison, logical operator, or conditional absorbs it.
- Course-defined evaluation errors only where evaluation reaches
  (250-eval-order; `&&`/`||` short circuit, `?:` evaluates the chosen
  branch): division/modulus right operand zero; INT64_MIN / -1 and % -1
  (500-integer-overflow); shift count negative (in the right operand's
  own type) or >= 64.

## Architecture Review

Findings of the post-implementation audit (see pa3/audit.md for the full
probe evidence):

- The original implementation followed the README literally: per-line
  isolation, trailing tokens rejected, `!` yielding signed int, static
  two-pass typing, `defined` substitution as a whole-line pre-pass.
  Differential fuzzing against `ctrlexpr-ref` showed the reference
  diverges on all of these; ~95% of random fuzz files mismatched. The
  implementation was rewritten around the streaming
  `CtrlExprCalculator` + single-pass `Evaluate` described above; the
  same fuzzing now passes clean (see Validation in audit.md).
- The unsigned-zero recovery and its event algebra cannot be expressed
  as a static type pass: a recovery's signedness depends on whether
  evaluation reaches it (`0 ? == : 4` -> 4 but `1 ? == : 4` -> 0u), so
  type and value are computed in one walk with an `enabled` flag, which
  is also exactly what the short-circuit error rules need.
- Provenance matters: an invalid pp-number, an invalid character
  literal, and an invalid operator all become PTK_INVALID, but behave
  differently (carry vs poison). The calculator therefore takes the
  emit channel as a `TokenCategory` next to the analyzed token rather
  than re-deriving facts from spellings downstream.
- The reference accepts GNU binary literals (`0b101` is `int 5` in
  posttoken-ref too). That was a PA2-level gap in `numeric_literals`,
  fixed there (shared ownership: the ctrlexpr layer contains no number
  grammar of its own beyond the err-vs-poison shape split, which is a
  ctrlexpr-only behavior).

## Final Architecture Review

- Ownership boundaries hold: phases 1-3 are reused byte-identically
  (`IPPTokenStream` is the only coupling); literal typing lives in
  `numeric_literals`/`text_literals` (PA2); the controlling-expression
  grammar, the pinned line-state machine, and the recovery algebra live
  in `ctrl_expr.cpp`; the `defined` answer stays behind `IsDefinedFn`
  with the mock in the driver. `EvaluateControllingExpression` drives
  the same calculator over one token vector, so PA6 reuses the exact
  semantics without the stream.
- No test-specific gates, fallback success paths, or embedded outputs:
  every output line is computed by the parser/evaluator; `error` lines
  all flow from the same state machine; phase 1-3 failures remain hard
  EXIT_FAILURE in the driver only.
- Performance: per token O(1) amortized (one PostToken copy into the
  unit, one AST node per grammar node, single evaluation walk); per line
  O(tokens); no rescans of the suite, no quadratic behavior. The carried
  unit is swapped out, not copied.
- Validation: 103/103 `make test-report-through-pa3` (PA1/PA2 suites
  green, 4 new PA3 fixtures + 1 new PA2 fixture pin the audit findings);
  differential fuzz vs `ctrlexpr-ref`, `posttoken-ref`, and
  `pptoken-ref` clean on fresh seeds (see audit.md).
