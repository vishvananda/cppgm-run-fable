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

- `dev/src/utf8.{h,cpp}` — single-character UTF-8 decoding (sequence
  structure enforced; code point range deliberately not: overlong forms and
  surrogates decode, and stray 0x80-0x9F bytes fall back to Windows-1252,
  all reference behavior) and UTF-8 encoding of code points. Owned here
  because later phases (literal evaluation, output writers) need the same
  conversions.
- `dev/src/source_translation.{h,cpp}` — phases 1 and 2, interleaved at the
  byte level (the interleaving is observable, see below). Produces a
  `TranslatedSource` value:
  - `bytes`: the source file image with the missing final line feed
    appended.
  - `chars`: the translated stream (UTF-8 decode combined with trigraphs,
    line splices, UCNs; a leading BOM is dropped). Each translated code
    point records the half-open range of `bytes` offsets it was produced
    from. Malformed bytes become in-band `kInvalidChar` entries rather than
    immediate errors, because a bad byte is only ill-formed if tokenization
    reads it in translated mode.
  The source mapping is the ownership boundary that lets raw string literals
  revert translations exactly: the tokenizer rescans `bytes` for the raw
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

Order: append missing final LF to the byte stream -> one combined
left-to-right byte-level pass for UTF-8 decoding, trigraphs, line splices,
and UCNs. The pass reproduces the reference pipeline's semantics, which were
established empirically against `pptoken-ref` (fixtures plus targeted probes
and three differential fuzz corpora, 12k cases):

- Phases 1 and 2 are interleaved at the byte level, and the interleaving is
  observable: lookahead consumed after a backslash, after a lone `?`, or
  after a splice is read as a single raw byte, identity-mapped to a code
  point (byte 0x93 becomes U+0093, not its Windows-1252 meaning, and a
  multi-byte UTF-8 sequence is split, usually leaving ill-formed
  continuation bytes behind). An eager decode-then-translate pipeline gets
  all of these wrong.
- Normal-mode UTF-8 decoding enforces sequence structure but not code point
  range (overlong forms and surrogates decode), and stray 0x80-0x9F bytes
  fall back to their Windows-1252 meaning (the pa34 hosted-compat fixtures
  depend on this). Out-of-range code points are rejected only by the UTF-8
  *encoder*, i.e. only when they reach emitted token data.
- Malformed bytes are not translation-time errors: they become in-band
  `kInvalidChar` entries that throw only when the tokenizer reads them in
  translated mode. Inside a raw string literal (rescanned from bytes) or
  after the in-band end-of-input sentinel they are unreachable and harmless,
  matching the reference.
- `??X` forming a trigraph translates; `??` without a trigraph terminal
  re-examines from the second `?`; `?X` emits `?` and consumes the next
  byte raw.
- A backslash followed by `u`/`U` scans a raw hex quad: on success the code
  point is decoded (`\UFFFFFFFF` becomes the in-band end-of-input sentinel,
  which stops tokenization); on failure the backslash, the `u`/`U`, the
  scanned prefix, and the stopping byte are all consumed raw. Any other
  byte after a backslash is consumed with it, so `\??/` never forms a
  trigraph and `\\` + newline never splices.
- After a splice the next byte is consumed raw: only a backslash re-enters
  translation; a `?` there never starts a trigraph.
- A leading BOM is dropped, and the character after it is decoded normally
  (multi-byte sequences and the Windows-1252 fallback apply) but consumed
  without phase-2 interpretation: it never starts a trigraph, splice, or
  UCN. A BOM anywhere else is an ordinary U+FEFF.
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
  validation, max 16 decoded characters, newline/EOF is an
  unterminated-literal error); scanning happens over `bytes` so
  trigraphs/splices/UCNs are reverted, with ordinary UTF-8 decoding (and
  its Windows-1252 fallback) applied. The closing `)delim"` is matched by
  decoded code point, not byte-wise, so a stray legacy byte and its UTF-8
  spelling are the same d-char. The token text is prefix + raw slice
  re-encoded.
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

## Architecture Review

The audit pass (see `pa1/audit.md`) found one structural defect in the
original implementation: it ran phase 1 eagerly (whole-file UTF-8 decode to
a code point vector) and phase 2 over the decoded code points. Differential
fuzzing against `pptoken-ref` showed the reference interleaves the phases at
the byte level, and the difference is observable wherever translation
consumes lookahead — after a backslash, a lone `?`, a splice, or a leading
BOM — and wherever a malformed byte sits in a region tokenization never
reads in translated mode (raw string interiors, text after `\UFFFFFFFF`).
47 of 3000 fuzz cases accepted input the reference rejects, and several
accepted cases emitted differently-spelled token data.

The translator was rewritten to walk the byte stream directly:

- `TranslatedSource.raw` (code points) became `TranslatedSource.bytes` (the
  byte image); `TranslatedChar` source ranges became byte ranges. The
  raw-string rescan and resume logic carried over unchanged in structure.
- Consumed lookahead is emitted as a single identity-mapped byte
  (`EmitByte`), never decoded and never re-entering trigraph/splice/UCN
  recognition.
- Decode failures emit an in-band `kInvalidChar` instead of throwing;
  the tokenizer's single `At()` accessor throws when one is actually read
  in translated mode. Raw string scanning decodes from `bytes` on demand,
  so poisoned entries inside raw literals are skipped by construction.
- The leading-BOM rule (drop BOM, consume the next character without
  phase-2 interpretation) lives in the translator's entry point.

Ownership boundaries after the rewrite: `utf8` owns byte-level
decode/encode policy (structure-only validation, Windows-1252 fallback,
encoder range check); `source_translation` owns phase-1/2 interleaving,
consumption semantics, and the byte-range source map; `pp_tokenizer` owns
token classification, the header-name context machine, and raw-string
delimiter matching over decoded characters. No facts are re-derived
downstream: the tokenizer consumes only `chars`, `bytes`, and the source
map.

## Final Architecture Review

Confirmed against the rewritten implementation:

- No skipped phases, fallback success paths, dummy output, or
  test-specific gates; the tool always runs decode + translate + tokenize
  over stdin and the only success path is a fully tokenized stream.
- All scans are linear: the translator visits each byte once, the
  tokenizer visits each translated char a bounded number of times
  (operator munch lookahead <= 4, delimiter match <= 17 decoded chars per
  `)` candidate). Measured: 2 MB realistic source in ~0.4 s CPU; a 2 MB
  adversarial raw string of `)` candidates in ~0.1 s; no quadratic
  behavior.
- The source map costs 24 bytes per translated char (transient, freed at
  exit). At course input scale (kilobytes) this is negligible; it is the
  price of exact raw-string reversion and resume.
- Validation: 49/49 through-pa1 tests, file audit clean, ~70 targeted
  reference probes, 12k differential fuzz cases across three seeded
  corpora (byte-exact output and exit-status agreement), and
  self-tokenization of all five implementation sources byte-identical to
  `pptoken-ref`.
