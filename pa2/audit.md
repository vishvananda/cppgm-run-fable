# PA2 posttoken — audit

## Audit Plan

Scope: the PA2 commit b7e0d2ae4 ("Implement PA2 posttoken") against
`pa2/plan.md`, the assignment README, and the PA1 baseline (3a033ce82).

Files to inspect:

- `dev/posttoken.cpp` — tool entry; verify it runs the full phase 1-7
  pipeline (no shortcut around `TranslateSource`/`TokenizePPTokens`) and
  keeps PA1 error behavior (stderr diagnostic + `EXIT_FAILURE`).
- `dev/src/post_token.{h,cpp}` — token model, keyword/operator tables,
  output formatting; verify the tables match the README's course list
  exactly and `DescribePostToken` is the only place the PA2 line format is
  produced.
- `dev/src/post_tokenizer.{h,cpp}` — phase-6 string-sequence buffering;
  verify whitespace/new-line do not break sequences, every non-string
  emit flushes first, and eof flushes before emitting `eof`.
- `dev/src/numeric_literals.{h,cpp}` — pp-number classification (2.14.2 /
  2.14.4 grammars, Table 6 ladder on LP64, hex-float extension, the
  reference's UD shape scanner); look for grammar gaps, overflow
  mishandling, duplicated scan logic, and dead generality.
- `dev/src/text_literals.{h,cpp}` — char/string literal analysis; verify
  re-parsing of PA1 spellings cannot mis-split (escaped quotes, raw
  delimiters, ud-suffixes), escape decoding matches the pinned wrapping
  behavior, and the encoding error path stays a hard error.
- `dev/src/lex_char_classes.{h,cpp}` — shared classification; verify the
  extraction from `pp_tokenizer.cpp` changed no semantics (PA1
  regression risk).
- `dev/src/pp_tokenizer.cpp`, `dev/src/utf8.{h,cpp}`,
  `dev/src/source_translation.*` — confirm PA1 behavior untouched apart
  from the moved character classes.
- `dev/frontend_source_sets.mk`, `pa2/Makefile` — confirm `posttoken`
  links the real pipeline objects and tests run both `pa2/tests` and the
  course suite.

Cheating-pattern checks:

- No test-specific or source-shape acceptance gates (search for fixture
  names, input-size switches, hard-coded outputs).
- No fallback success paths: conversion failures must produce `invalid`
  tokens, not silently guessed output; phase 1-3 errors must stay
  `EXIT_FAILURE`.
- Output must come from real analysis (typed values + ABI byte dumps),
  not embedded reference payloads. Validate by differential fuzzing
  against `posttoken-ref` with fresh seeds not used during development.

Performance risks to inspect:

- Per-token allocation patterns (`IntegerTypeCandidates` vector, string
  sequence buffering, `DescribePostToken` ostringstream) — bounded and
  linear per token, but check for hidden quadratic behavior on long
  literals or long string sequences.
- `posttoken.cpp` per-line `endl` flush (matches PA1 tool style; output
  size is small — verify no full-suite impact).

Ownership boundaries to verify:

- `IPPTokenStream` (course-defined) is the only coupling between the PA1
  tokenizer and PA2 analysis; literal spellings are re-parsed downstream
  because the course interface passes spellings — confirm the re-parse
  is total (cannot disagree with PA1's scan) rather than a duplicated
  grammar that can drift.
- Character classification must have a single owner
  (`lex_char_classes`); no leftover copies in `pp_tokenizer.cpp`.
- Keyword/operator tables must have a single owner (`post_token.cpp`)
  and `pp_tokenizer.cpp`'s operator sets must stay phase-3 concerns
  (different token sets, so duplication is semantic, not textual).

File-audit issues:

- `perl scripts/cppgm_file_audit.pl --stage pa2 --paths dev/src` must
  pass with no exemptions added; check no implementation fragments were
  moved outside `dev/src` to dodge the audit (e.g. into headers under
  other paths or generated files).

## Findings

The structural review found no cheating patterns: the tool runs the full
phase 1-7 pipeline, failures come from real analysis, no test-specific
gates or embedded payloads exist, and the file audit passes without
exemptions. However, a fresh-seed differential fuzz against
`posttoken-ref` (4500 cases, seeds never used during implementation)
found 130+ mismatches in six behavioral classes the original ~1900-case
fuzz had missed:

1. **Float range divergence** (the largest class, ~120 cases). The
   implementation scanned floats with modern `istringstream`, which
   clamps out-of-range values to the largest finite value; the
   reference's era of libstdc++ stored the raw `strtof/strtod/strtold`
   result, so `1e999f` must print as +infinity (`0000807F`), not
   `FLT_MAX`. Underflow likewise: denormals and zeros come straight
   from the C library.

2. **String sequences survive body-invalid character literals.**
   `"a" '' "b"` concatenates to `"a" "b"` around an inline `invalid ''`.
   Only character literals whose *body* fails (empty, multiple code
   points, out-of-range code point) behave this way; suffix and
   code-unit-width failures (`'b'q`, `u'\x10000'`), invalid pp-numbers,
   invalid operators (`#`), and non-whitespace characters all terminate
   the sequence normally. Probes p01-p16, p35, p70-71, p103-104,
   p110-111 pinned the exact rule.

3. **Text-literal ud-suffixes are extracted by ASCII-only forward scan.**
   The reference scans `[A-Za-z0-9_]` after the closing quote; the
   non-ASCII identifier characters PA1 legitimately attached are dropped
   from analysis (but stay in the source): `"abc"é` is a *plain* string
   literal, `"abc"_9é` is a UDL with suffix `_9`, while `"abc"xé` is
   invalid (ASCII suffix `x` without `_`). The dropped form is also what
   concatenation compares. pp-numbers differ: their suffix splits at the
   first `_` and keeps non-ASCII characters (`123_é` → `_é`).

4. **String-sequence check order.** Prefix conflicts and suffix
   conflicts produce `invalid` before any encoding (`u8"\x10FFFF" u"x"`
   is invalid, not an error); the UTF-8 encode error is a hard
   EXIT_FAILURE that precedes the suffix-form check (`"\x10FFFF"s`
   errors rather than printing invalid); encoding always uses the
   sequence-wide prefix (`u"x" "\x10FFFF"` is a valid UTF-16 array).

5. **UD pp-number shape scanner gaps.** The reference's DFA hunts a
   dangling exponent marker across arbitrary suffix characters with the
   x→hex switch active (`5E_bhE8`, `1e_xp3`, `1e_exp3` floating;
   `1e_x`, `1e_ea3` invalid); exponents continue with base digits after
   the first decimal digit (`0x1p3c_l`, `0x1p+f_l` floating, `0x1pf_l`
   invalid) and accept one dot after the marker, sign, or digits
   (`164e._x`, `1e+._`, `0x1p.c_l` floating; second dots invalid).

6. **PA1 regression: pp-number extent.** `8E0e4x0e-7968ecf` is one
   pp-number in the reference but split by our `ScanPPNumber`: a marker
   takes a sign only when a decimal digit or sign directly follows it,
   and x/X switches to hexadecimal only before any dot *and* before any
   completed decimal exponent (`12x3p+4`, `1eAx2p+3` one token;
   `1.2x3p+4`, `1e+x2p+3`, `1E0x2p+3` split). Pinned by pptoken-ref
   probes; this affected PA1 output too.

Cleanups found by inspection: dead `allow_suffix` parameters (always
true), duplicated float byte-packing between the decimal and hex paths,
and `IsUdSuffixSpelling` left orphaned by finding 3. Performance review
found no quadratic scans, repeated suite walks, or hot-path
recomputation; per-token work is linear in the spelling.

## Changes Made

- `dev/src/numeric_literals.cpp`: floats now scan via
  `strtof/strtod/strtold` with a shared `ValueObjectBytes` packer
  (fixing finding 1, absorbing the former duplicated
  `MakeHexFloatingLiteral`, and dropping the `allow_suffix`
  parameters); `UdNumberShapeScanner` gained the hunting/x-switch rules
  in `kSufHunt`/`kSufHuntMark`, base-digit exponent continuation, and
  dot transitions from `kExpMark`/`kExpSign` (finding 5).
- `dev/src/text_literals.{h,cpp}`: `ExtractAsciiUdSuffix` implements
  finding 3 for both literal kinds; `AnalyzeCharLiteral` returns
  `CharLiteralAnalysis` with the `body_invalid` stage bit (finding 2)
  and checks the body before the suffix; `AnalyzeStringSequence`
  reorders to conflicts → encode → suffix-form (finding 4).
- `dev/src/post_tokenizer.{h,cpp}`: `EmitCharLiteral` flushes the
  pending string sequence only for non-body failures (finding 2).
- `dev/src/pp_tokenizer.cpp`: `ScanPPNumber` rewritten to the
  marker/sign/x-switch rule (finding 6).
- `dev/src/lex_char_classes.{h,cpp}`: removed the now-unused
  `IsUdSuffixSpelling` and its `utf8.h`/`<string>` dependencies.
- `pa2/tests/500-560`: seven new regression tests pin findings 1-6 with
  reference outputs generated by `make -C pa2 ref-test`; existing test
  refs are untouched.
- `pa2/plan.md`: pinned-semantics section rewritten to match the
  reference behavior; Architecture Review and Final Architecture Review
  added.

## Validation

- 85 targeted probes (flush rules, suffix extraction, check order, hunt
  and exponent shapes, float ranges, pp-number sign shapes) byte-match
  the reference binaries.
- Differential fuzz, fresh seeds 7001-7003 (3×1500 cases) and
  edge-targeted seeds 8001-8002 (2×2000 cases with non-ASCII suffixes
  and number-shape stress): 0 mismatches in stdout and exit status for
  both `posttoken` vs `posttoken-ref` and `pptoken` vs `pptoken-ref`
  (the latter guards the PA1 `ScanPPNumber` change).
- `make test-report-through-pa2`: 79/79 (72 original + 7 new), PA1
  suite green.
- `perl scripts/cppgm_file_audit.pl --stage pa2 --paths dev/src`:
  passed, 22 files checked, no exemptions added.
