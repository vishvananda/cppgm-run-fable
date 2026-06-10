# PA1 (pptoken) Implementation Plan

## Goal

Implement translation phases 1-3 of the C++11 standard (N3485 clause 2) as the
real front end of the staged compiler: UTF-8 decoding, trigraph replacement,
line splicing, universal-character-name decoding, and decomposition into the
eleven pptoken kinds emitted through `IPPTokenStream`.

## Architecture and ownership

The tokenizer is the first stage of the compiler and is reused by every later
tool (posttoken, preproc, ... cppgm++), so the implementation lives in shared
modules under `dev/src/`, with `dev/pptoken.cpp` as a thin tool entry point.

- `dev/src/utf8.{h,cpp}` — strict UTF-8 decoding (rejects overlong forms,
  surrogates, out-of-range and truncated sequences) and UTF-8 encoding of code
  points. Owned here because later phases (literal evaluation, output writers)
  need the same conversions.
- `dev/src/source_translation.{h,cpp}` — phases 1 and 2. Produces a
  `TranslatedSource` value:
  - `raw`: the phase-1 code point stream (UTF-8 decoded, leading BOM dropped,
    missing final line feed appended).
  - `chars`: the fully translated stream (trigraphs, line splices, UCNs, plus
    a synthetic final line feed if splicing consumed the last one). Each
    translated code point records the half-open range of `raw` indices it was
    produced from.
  The source mapping is the ownership boundary that lets raw string literals
  revert translations exactly: the tokenizer rescans `raw` for the raw
  portion, then resumes the translated stream after the closing quote.
- `dev/src/pp_tokenizer.{h,cpp}` — phase 3. An index-based scanner over
  `TranslatedSource.chars` with arbitrary lookahead (needed for `<::`
  disambiguation, `.` vs pp-number, encoding prefixes, ud-suffixes). Emits to
  `IPPTokenStream`. Owns the Annex E1/E2 identifier tables, the operator
  table, and the `#include` header-name context tracking.
- `dev/pptoken.cpp` — reads stdin, runs translation + tokenization into
  `DebugPPTokenStream`, maps thrown diagnostics to `EXIT_FAILURE`.

New `dev/src` sources are registered in `dev/frontend_source_sets.mk` under
`FRONTEND_OBJ_BASENAMES_pptoken`. Later assignments add the same basenames to
their tools when they start consuming the token stream.

## Translation pipeline decisions

Order: UTF-8 decode -> BOM strip -> append missing final LF -> one combined
left-to-right pass for trigraphs, line splices, and UCNs. The combined pass
reproduces the reference pipeline's lookahead-consumption semantics, which
were established empirically against `pptoken-ref` (fixtures plus targeted
probes and differential fuzzing):

- UTF-8 decoding enforces sequence structure but not code point range, and
  stray 0x80-0x9F bytes fall back to their Windows-1252 meaning (the pa34
  hosted-compat fixtures depend on this). Out-of-range code points are
  rejected only by the UTF-8 *encoder*, i.e. only when they reach emitted
  token data; a bad code point inside a comment is harmless.
- `??X` forming a trigraph translates; `??` without a trigraph terminal
  re-examines from the second `?`; `?X` consumes both characters
  untranslated.
- A backslash followed by `u`/`U` scans a raw hex quad: on success the code
  point is decoded (`\UFFFFFFFF` becomes the in-band end-of-input sentinel,
  cp -1, which stops tokenization); on failure the backslash, the `u`/`U`,
  the scanned prefix, and the stopping character are all consumed
  untranslated. Any other character after a backslash is consumed with it,
  untranslated, so `\??/` never forms a trigraph and `\\` + newline never
  splices.
- After a splice the next character is read raw: only a backslash re-enters
  translation; a `?` there never starts a trigraph.
- The missing final LF is appended once, before splicing; a trailing splice
  that consumes it is not restored (`foo\` lexes to just `foo`).
- Errors are thrown as exceptions and reported with `EXIT_FAILURE`; on
  non-success exits the harness compares status only.

## Tokenizer decisions

- Whitespace sequences subsume comments; a multi-line block comment swallows
  its interior newlines (one `whitespace-sequence` token, fixture-confirmed).
  Unterminated block comments are errors, and so are line comments whose
  terminating newline was consumed by a trailing splice. No
  `isspace()`/locale use; explicit space/tab/vtab/formfeed/CR membership
  only.
- Identifiers: initial char is a nondigit or an E1-minus-E2 code point;
  continuation chars are nondigit/digit/E1. A complete identifier equal to an
  encoding prefix (`u8 u U L` + `"`; `u U L` + `'`; `R uR u8R UR LR` + `"`)
  continues into the literal scanner; identifier-like operators (`new`,
  `and_eq`, ...) are re-emitted as `preprocessing-op-or-punc`.
- Character/string literals validate escape sequences (simple, octal, hex)
  and reject newline/EOF inside the literal. Empty character literals are
  accepted (reference behavior). After the closing quote an
  identifier-initial char starts a ud-suffix.
- Raw strings: delimiter is everything up to the first `(` (no d-char
  validation, max 16, newline/EOF is an unterminated-literal error);
  scanning happens over `raw` so trigraphs/splices/UCNs are reverted. The
  token text is prefix + raw slice re-encoded.
- Header names only after (line start) (`#` or `%:`) `include`, tracked as a
  small token-level state machine where whitespace tokens do not reset state;
  `%:%:` is not `%:`. A header name not closed before the newline is an
  error (reference behavior).
- `<::` followed by something other than `:`/`>` lexes as `<` + `::`
  (2.5p3); otherwise normal longest-munch from the operator table.
- pp-numbers: `.` and identifier-nondigits continue the number; the
  exponent character that may consume a following sign is `e`/`E` normally
  but `p`/`P` when the number's first `x`/`X` precedes its first `.`
  (hexadecimal literals, reference behavior).
- Any other non-whitespace code point is a `non-whitespace-character` token,
  except `'` and `"` which are course-defined errors when unmatched.

## Validation

- `make test-pa1` for the assignment suite, plus
  `make test-report ACTIVE_TEST_REPORT_PAS='pa1'` for scoped diagnosis.
- Exit gate: `make test-report-through-pa1` clean and
  `perl scripts/cppgm_file_audit.pl --stage pa1 --paths dev/src` passing.
