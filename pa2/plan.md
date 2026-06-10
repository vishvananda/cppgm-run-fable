# PA2 posttoken — design plan

## Goal

`posttoken` runs translation phases 1-6 plus the tokenization half of phase 7:
it reuses the PA1 pipeline (phases 1-3) to obtain `preprocessing-tokens`, then
converts each into a `token` (simple / identifier / literal /
user-defined-literal / invalid) and prints the PA2 debug format.

Phase 1-3 errors keep PA1 behavior: diagnostic on stderr, `EXIT_FAILURE`.
Errors converting an individual preprocessing-token produce one `invalid`
token and processing continues (all PA2 tests exit `EXIT_SUCCESS`).

## Ownership boundaries (dev/src modules)

The PA1 stack (`source_translation`, `pp_tokenizer`, `utf8`) is reused as-is
through `IPPTokenStream`. New modules:

- `lex_char_classes.{h,cpp}` — character classification shared by the PA1
  tokenizer and PA2 literal analysis: digits, hex/octal digits, identifier
  start/continue (Annex E ranges), simple-escape membership. Extracted from
  `pp_tokenizer.cpp` so ud-suffix validation does not duplicate the Annex E
  tables.
- `post_token.{h,cpp}` — the typed token model used by PA3+ as well:
  `EFundamentalType`, `ETokenType` (simples), `PostToken` (kind, source,
  typed value bytes, array length, ud-suffix/prefix), `IPostTokenStream`
  sink interface, keyword/operator tables, and `DescribePostToken` which
  renders the canonical PA2 line (hexdump included).
- `numeric_literals.{h,cpp}` — pp-number analysis: split ud-suffix at the
  first `_` (a pp-number ud-suffix begins with an underscore and keeps
  every later character, non-ASCII included), match integer-literal /
  floating-literal grammar (2.14.2 / 2.14.4), compute integer value with
  overflow detection and the Table 6 type ladder on the LP64 ABI, scan
  floats with strtof/strtod/strtold exactly like the reference (see
  below). UD integer/floating keep the unscanned prefix (no range
  check).
- `text_literals.{h,cpp}` — character-literal and string-literal analysis:
  encoding-prefix/raw/body parsing, ud-suffix extraction by forward scan
  of ASCII identifier characters after the closing quote (non-ASCII
  identifier characters PA1 attached stay in the source but are dropped
  from the analysis), escape-sequence decoding to code points, course
  Unicode range validation ([0,0xD800) ∪ [0xE000,0x110000)) for char
  literals, char-literal typing (char/int for ordinary, one code unit
  for u/U/L), phase-6 concatenation over a maximal string sequence
  (prefix and ud-suffix compatibility rules in the reference's check
  order), and UTF-8/16/32 encoding of the array value.
  `AnalyzeCharLiteral` reports whether a failure happened in the body
  (`CharLiteralAnalysis::body_invalid`) because that distinction drives
  string-sequence flushing.
- `post_tokenizer.{h,cpp}` — `PostTokenizer : IPPTokenStream`: ignores
  whitespace/new-line, buffers maximal string-literal sequences and
  flushes them on any other token or eof — except a character literal
  whose body fails to decode, which the reference reports inline without
  breaking the sequence — maps identifiers through the keyword table,
  ops through the operator table (`#`/`##`/`%:`/`%:%:` invalid),
  delegates pp-numbers and char literals, emits `invalid` for
  header-names and non-whitespace-characters.
- `dev/posttoken.cpp` — thin tool entry mirroring `pptoken.cpp`: cin →
  `TranslateSource` → `TokenizePPTokens` → `PostTokenizer` → a printing
  `IPostTokenStream` over `DescribePostToken`.

`dev/frontend_source_sets.mk` gains the new objects for `posttoken` (and
`lex_char_classes` for `pptoken`).

## Semantics pinned by refs (fixtures + reference-binary observation)

- A text-literal ud-suffix is the run of ASCII identifier characters
  after the closing quote: `"abc"yyy` and `'a'xé` are invalid (ASCII
  suffix without leading `_`), `"abc"é` and `'a'é` are plain literals
  (non-ASCII junk dropped silently), `"abc"_9é` is a UDL with suffix
  `_9`. The truncated suffix is what concatenation compares
  (`"a"_xé "b"_x` has the single suffix `_x`). pp-number suffixes are
  different: they split at the first `_` and keep non-ASCII characters
  (`123_é` → suffix `_é`).
- A string sequence is checked in this order: more than one distinct
  encoding-prefix or more than one distinct ud-suffix → one `invalid`
  (sources space-joined) before any encoding; then the array is encoded
  (UTF-8 range failure = hard error, even if the suffix is bad:
  `"\x10FFFF"s` is EXIT_FAILURE not `invalid`); only then must the
  suffix start with `_` (`""s ""s` → invalid after encoding).
- A character literal whose *body* fails — empty, more than one code
  point, or out of the course range — is reported `invalid` immediately
  and does NOT terminate a string sequence (`"a" '' "b"` concatenates
  around the inline `invalid ''`); suffix and code-unit-width failures
  (`'b'q`, `u'\x10000'`) flush the sequence like any other token.
- Integer ladder exactly Table 6 (decimal unsuffixed never goes unsigned;
  out of range → `invalid`). Hexdump width = sizeof chosen type (4/8).
- Floats scan with strtof/strtod/strtold semantics: the reference's
  libstdc++ stored the C-library result directly, so out-of-range
  literals print as ±infinity (`1e999f` → `0000807F`) and underflows as
  zero/denormals — the modern istringstream extraction clamps to the
  largest finite value instead and must not be used. strtod also covers
  the C99 hexadecimal forms (`0x1.8p1`), which accept only `l`/`L` as a
  suffix (`0x1p3f` invalid).
- A pp-number containing a ud-suffix is validated by the reference's
  scanner, which keeps running its number DFA inside the suffix
  (`numeric_literals.cpp` UdNumberShapeScanner documents the quirks:
  `123_e3` invalid / `123_ex` valid / `123_xe3` valid via hex switch /
  `1e_e3` floating with prefix `1e` / a dangling marker hunts across
  arbitrary suffix characters for marker-digit, x-switching on the way
  (`5E_bhE8`, `1e_xp3`, `1e_exp3` floating; `1e_x`, `1e_ea3` invalid) /
  exponents continue with base digits after the first decimal digit
  (`0x1p3c_l` floating) and may take one dot (`164e._x`, `0x1p.c_l`) /
  `078_x` invalid but `078e2_x` floating).
- PA1's pp-number scanner takes a sign after an exponent marker only
  when a decimal digit or sign directly follows the marker, and an x/X
  switches the marker to p/P only before any dot or completed decimal
  exponent (`12x3p+4`, `8E0e4x0e-7968ecf`, `1eAx2p+3` stay one token;
  `1.2x3p+4`, `1e+x2p+3`, `1E0x2p+3` split at the sign).
- Ordinary char literal: cp ≤ 127 → `char` (1 byte) else `int` (4 bytes);
  `u` requires one UTF-16 code unit (`u'𝄞'` invalid); `U`/`L` are 4-byte.
  Multi-code-point or empty char literals → `invalid`. Char literals are
  range-checked against [0,0xD800) ∪ [0xE000,0x110000).
- Hex escapes accumulate into a wrapping 32-bit value (`'\x100000000'`
  is NUL). String code points are NOT range-checked: lone surrogates
  encode raw in all encodings, UTF-16 runs the surrogate-pair bit-ops
  with 16-bit truncation for any value ≥ 0x10000 (unsigned), UTF-32 dumps
  raw 32-bit values, and only the UTF-8 encoder rejects (≥ 0x10FFFF),
  which is a hard translation error (EXIT_FAILURE), not an invalid token.
- Raw string bodies decode literally (no escape processing); source
  spelling retains the full `R"delim(...)delim"` form from PA1.
- stderr is not compared; stdout and exit status are.

Differential validation: ~1900 generated cases during implementation,
plus 8500 fresh-seed cases (including non-ASCII-suffix and number-shape
targeted generators) during the audit, byte-compared against
`posttoken-ref` *and* `pptoken-ref` including exit statuses; regression
tests 460-490 pin hex-float, ud-shape, and encoding edges, and 500-560
pin the audit discoveries (flush behavior, ASCII suffix extraction,
string check order, float ranges, hunt/exponent shapes, pp-number sign
shapes), with refs generated through `make -C pa2 ref-test`.

## Validation

- `make test-pa2` then root `make test-report-through-pa2` (the through
  check is the exit criterion; PA1 must stay green).
- `perl scripts/cppgm_file_audit.pl --stage pa2 --paths dev/src` clean:
  functions ≤ 120 lines, nesting ≤ 6, no stub/shortcut markers.

## Architecture Review

Ownership is single-homed and matches the module plan above:

- `IPPTokenStream` (the course-defined PA1 interface) is the only
  coupling between phase 1-3 and phase 7. It passes token spellings, so
  the literal analyzers re-derive structure (prefix, body, suffix) from
  the spelling; that re-parse is total over PA1-validated tokens — every
  branch that "cannot happen" for PA1 output is still handled (as
  `invalid`) rather than asserted, and no PA1 internals leak across the
  boundary.
- Character classification lives only in `lex_char_classes` (the
  PA1-era copies in `pp_tokenizer.cpp` were removed when the module was
  extracted); the keyword/operator/type-name tables live only in
  `post_token.cpp` as function-local statics built once; the PA2 output
  format is produced only by `DescribePostToken`.
- The reference's two-stage character-literal failure semantics is
  represented as data (`CharLiteralAnalysis::body_invalid`) computed by
  the analyzer that knows it, and merely consumed by `PostTokenizer` —
  the sequencing policy never re-derives literal facts from spellings.
- `pp_tokenizer.cpp`'s `ScanPPNumber` was corrected during the audit to
  the reference's marker/sign/x-switch rule; this is a phase-3 fact
  (token extent) owned by phase 3 — posttoken sees the corrected
  pp-token and needs no compensation downstream.

Performance: every analysis is a single linear pass over the token
spelling. The string-sequence buffer is swapped out on flush (no
copies), table lookups are O(1) hash hits or O(log n) enum-to-name maps
on the output path, and the only per-token heap traffic is the small
candidate vector in `IntegerTypeCandidates` and transient substrings —
all bounded by the token length. No full-suite rescans, no quadratic
behavior on long literals or long concatenation sequences.

## Final Architecture Review

Re-checked after the audit fixes landed:

- No fallback success paths: phase 1-3 errors still throw and exit
  EXIT_FAILURE via the tool entry; conversion failures construct
  `invalid` tokens only from real analysis outcomes; the UTF-8 encode
  error deliberately remains a hard error matching the reference.
- No test-specific gates, dummy outputs, or embedded payloads: outputs
  are computed from the token model (typed values + little-endian byte
  packing); behavior was validated against the reference binary on 8500
  fresh fuzz inputs the test suite has never seen, for both `posttoken`
  and `pptoken` (PA1 regression check after the `ScanPPNumber` change).
- Dead generality removed: the unused `allow_suffix` parameters, the
  duplicated hex-float scanning path, and the orphaned
  `IsUdSuffixSpelling` helper are gone; float byte packing is shared by
  `ValueObjectBytes`.
- File audit passes with no exemptions; all implementation lives in
  `dev/src` with the thin `dev/posttoken.cpp` entry.
