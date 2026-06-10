# PA3 ctrlexpr — audit

## Audit Plan

Scope: the PA3 commit 03293e6ed ("Implement PA3 ctrlexpr") against
`pa3/plan.md`, the assignment README, and the PA2 baseline (eee156c9f).

Files to inspect:

- `dev/ctrlexpr.cpp` — driver; verify it runs the real phase 1-3
  pipeline (`TranslateSource` + `TokenizePPTokens`, no shortcut or
  per-line re-lexing), keeps phase 1-3 failures as stderr diagnostic +
  `EXIT_FAILURE`, and hosts only assignment-local state (the
  `PA3Mock_IsDefinedIdentifier` mock; verify its parity test is correct
  for non-ASCII lead bytes where `char` is signed).
- `dev/src/ctrl_expr.{h,cpp}` — all PA3 logic; verify:
  - `CtrlExprStream` line splitting matches the Features section (split
    at `new-line`, discard `whitespace-sequence`, empty line emits
    nothing, partial line at eof matches the reference's discard
    behavior pinned by `pa3/tests/400-end-of-input`);
  - token conversion is identifier_or_keyword context (no keyword
    folding) while reusing PA2's typed pp-number/char-literal analysis
    verbatim — no second literal grammar that can drift;
  - the parser is exactly the README grammar (10-level binary ladder,
    right-associative `?:`, both `defined` forms, trailing tokens
    rejected) with no acceptance shortcuts;
  - static signedness (16.2.4 promotion, usual arithmetic conversions,
    shift keeps left type, `?:` joins both branches statically) is
    separated from evaluation (short circuit, wrap semantics,
    course-defined error conditions only where evaluation reaches);
  - the course-defined evaluation errors are complete: div/mod right
    operand zero, INTMAX_MIN over `/ -1` and `% -1`, shift count
    negative (in the right operand's own type) or >= 64.
- `dev/frontend_source_sets.mk`, `pa3/Makefile` — ctrlexpr links the
  real PA1/PA2 objects; tests run both `pa3/tests` and the course
  suite.
- Shared PA1/PA2 sources (`pp_tokenizer`, `source_translation`, `utf8`,
  `lex_char_classes`, `post_token`, `numeric_literals`,
  `text_literals`) — the PA3 commit must not have changed their
  behavior (regression risk for `make test-report-through-pa1/pa2`).

Cheating-pattern checks:

- No test-specific or source-shape acceptance gates (search for fixture
  names, line-count switches, hard-coded expression results).
- No fallback success paths: a line that fails conversion, the token
  check, the parse, or evaluation must print `error` computed from real
  analysis — never a guessed number; phase 1-3 errors must stay
  `EXIT_FAILURE` and must not be downgraded to per-line `error`.
- No embedded reference payloads or output replay; validate behavior by
  differential fuzzing against `ctrlexpr-ref` with fresh seeds never
  used during implementation.
- Exception confinement claim in `plan.md` ("per-token analysis
  exceptions are confined to the line") must match the code: an
  analysis throw inside an emit handler would propagate to `main` and
  exit EXIT_FAILURE, so either the claim or the code needs fixing.

Performance risks to inspect:

- Per-line vector handling (`line_.swap`, per-token `PostToken` copies)
  — bounded and linear per token; check no per-token full-line rescans.
- Parser: one `Node` allocation per grammar node, 10-deep ladder
  recursion per primary — linear in tokens; confirm no backtracking
  (predictive parser) and no quadratic re-walks of the token vector.
- Evaluation: single post-order walk; static typing must not re-walk
  subtrees per operator.

Ownership boundaries to verify:

- `IPPTokenStream` stays the only coupling to phase 1-3;
  `EvaluateControllingExpression` takes converted tokens so the future
  preprocessor (PA6+) can evaluate `#if` lines without the line
  splitter — verify nothing in the evaluator reads driver state.
- `defined` resolution is a callback (`IsDefinedFn`); the evaluator
  must not own macro state, and the mock must live in the PA3 driver
  only.
- Literal typing/signedness facts must come from PA2's typed analysis
  (`EFundamentalType` + ABI value bytes), not re-derived from spellings
  (no stringly typing); identifier special cases (`true`, `false`,
  `defined`) are spelling-based by definition (16.1) and stay in the
  parser.

File-audit issues:

- `perl scripts/cppgm_file_audit.pl --stage pa3 --paths dev/src` must
  pass with no exemptions added; check no implementation fragments
  moved outside `dev/src` (driver must stay a thin entry point).

## Findings

The structural review found no cheating patterns: the tool runs the real
phase 1-3 pipeline (the PA3 commit touched no shared sources), output
comes from a real parser/evaluator, no test-specific gates or embedded
payloads exist, and the file audit passes without exemptions. However,
fresh-seed differential fuzzing against `ctrlexpr-ref` mismatched on
~95% of random files: the reference implements substantially different
semantics from the README's "invalid line prints error" description.
About 180 targeted probes (plus symbol-level inspection of the
unstripped reference, which exposed its `Calculator::accumulate` /
`try_calculate` streaming structure and `classify_int` integer parser)
pinned the following behavior classes, all initially divergent:

1. **Trailing tokens are ignored at top level.** `'a' int` prints 97;
   the parse just stops after a complete expression. But `)`/`:`
   mismatches inside constructs and end-of-line mid-parse stay hard
   errors, and the line check still rejects non-grammar tokens anywhere
   (`5 ,` is an error while `5 (` is 5).

2. **Recovery zeros.** A grammar operator where a primary was required
   is consumed and evaluates as a zero that is unsigned only where
   evaluation is enabled (`? 2 : 3` -> 0u, `> ? 1 : 2` -> 2,
   `1 ? == : 4` -> 0u but `0 ? == : 4` -> 4). A pending recovery in a
   binary right operand restores the left operand's signedness
   (`2 + == 4` -> 2 yet `% + 5` -> 5u, `2 + (5u + ==)` -> 7u vs
   `2 + (( == ) + 1)` -> 3); comparisons judge operands with pending
   recoveries discounted (`% - 9 > 0` signed, `% - 9u > 0` unsigned);
   comparisons, logical operators, and `?:` absorb pending events.

3. **`!` keeps its operand's signedness** (`!0u` -> 1u) — a divergence
   on completely valid inputs.

4. **Cross-line error state.** An invalid integer pp-number defers its
   `error` to the line's new-line while the unit keeps accumulating and
   carries into the next line (`5uu 7` then `5` evaluates `7 5` -> 7);
   float-shaped invalid spellings (dot, or exponent marker followed by
   digit/sign, with x switching the marker to p/P) poison the line like
   real floats; a deferred error plus a poison hits the next line too;
   token-less lines pass state through untouched.

5. **`defined` folds inline with stateful malformed handling.** Any
   non-folding token at the operand or `)` position defers an error and
   is consumed unanalyzed while the construct keeps waiting
   (`defined ( 3.5 a ) 6` folds to `1 6` and the next line continues
   the unit); a construct open at end of line defers the error and the
   folded unit carries; `defined` opens even on poisoned lines; the
   operand identifier is never rescanned (`defined defined a` -> 0).

6. **PA2 regression: binary literals.** The reference toolchain accepts
   GNU `0b`/`0B` binary literals (`0b101` is `int 5` in posttoken-ref,
   unsigned ladder like octal/hex, `0b101_x` is a UD integer with the
   strict digit/no-exponent body), which `numeric_literals` rejected.

Performance review found no quadratic scans, repeated suite walks, or
hot-path recomputation; per-token work is constant amortized and the
carried unit is swapped, not copied. Ownership review found one real
issue, fixed by the rewrite: distinguishing invalid-number from
invalid-character behavior requires emit-channel provenance, which the
analyzed PostToken alone cannot represent (the calculator now takes a
TokenCategory instead of re-deriving facts from spellings).

## Changes Made

- `dev/src/ctrl_expr.{h,cpp}`: rewritten around a streaming
  `CtrlExprCalculator` replicating the reference's accumulate/finish
  semantics (findings 1-5): evaluation units that survive deferred
  errors, one-line poison, inline defined folding, and the
  recovery-zero algebra implemented as a single `Evaluate` pass
  returning {value, is_unsigned, clean_unsigned, recovery} under an
  `enabled` flag. `EvaluateControllingExpression` now drives the same
  calculator (single owner) for the PA6 use case.
- `dev/src/numeric_literals.cpp`: binary-literal support (finding 6) in
  `MatchIntegerDigits`/`ComputeIntegerValue` and a binary mode in
  `UdNumberShapeScanner` (digits-only body, suffix machinery unchanged),
  pinned against posttoken-ref (`0b1_e1` invalid, `0b1_p1`/`0b1_x1` UD,
  `0b1_xp1` invalid).
- `pa3/tests/410-recovery`, `420-line-poison`, `430-error-carry`,
  `440-defined`: new regression fixtures pinning findings 1-5 with
  reference outputs generated via `make -C pa3 ref-test`.
- `pa2/tests/570-binary-literals`: pins finding 6 at the PA2 level.
- `pa3/plan.md`: rewritten to describe the actual architecture; pinned
  reference semantics catalogued; Architecture Review and Final
  Architecture Review added.

## Validation

- ~180 targeted probes (recovery typing, event absorption, defined
  machine positions, carry/poison interactions, binary literal ladder
  and UD shapes) byte-match the reference binaries.
- Differential fuzz with fresh seeds never used during implementation:
  200 bulk files (20,000 logical lines of expression/soup/comment/
  splice mix) plus 100 risky files (phase 1-3 errors, \UFFFFFFFF
  markers, missing final newlines), compared on stdout and exit status
  against `ctrlexpr-ref` — and the same corpus against `posttoken-ref`
  and `pptoken-ref` to guard the shared `numeric_literals` change: 0
  mismatches. (Earlier audit rounds covered a further 470 files, clean
  after the fixes.)
- `make test-report-through-pa3`: 104/104 (99 prior + 5 new), PA1/PA2
  suites green; pre-existing fixture refs byte-identical.
- `perl scripts/cppgm_file_audit.pl --stage pa3 --paths dev/src`:
  passed, no exemptions added.
