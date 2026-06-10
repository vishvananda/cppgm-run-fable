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
  first `_` (course rule: a ud-suffix begins with an underscore; the
  remainder must be a well-formed identifier), match integer-literal /
  floating-literal grammar (2.14.2 / 2.14.4), compute integer value with
  overflow detection and the Table 6 type ladder on the LP64 ABI, scan
  floats through the starter-code `istringstream` decode for bit-perfect
  output. UD integer/floating keep the unscanned prefix (no range check).
- `text_literals.{h,cpp}` — character-literal and string-literal analysis:
  encoding-prefix/raw/body/ud-suffix parsing, escape-sequence decoding to
  code points, course Unicode range validation ([0,0xD800) ∪
  [0xE000,0x110000)), char-literal typing (char/int for ordinary, one code
  unit for u/U/L), phase-6 concatenation over a maximal string sequence
  (prefix and ud-suffix compatibility rules), and UTF-8/16/32 encoding of
  the array value.
- `post_tokenizer.{h,cpp}` — `PostTokenizer : IPPTokenStream`: ignores
  whitespace/new-line, buffers maximal string-literal sequences and flushes
  them on any other token or eof, maps identifiers through the keyword
  table, ops through the operator table (`#`/`##`/`%:`/`%:%:` invalid),
  delegates pp-numbers and char literals, emits `invalid` for header-names
  and non-whitespace-characters.
- `dev/posttoken.cpp` — thin tool entry mirroring `pptoken.cpp`: cin →
  `TranslateSource` → `TokenizePPTokens` → `PostTokenizer` → a printing
  `IPostTokenStream` over `DescribePostToken`.

`dev/frontend_source_sets.mk` gains the new objects for `posttoken` (and
`lex_char_classes` for `pptoken`).

## Semantics pinned by refs (fixtures + reference-binary observation)

- ud-suffix must start with `_` for all UDL kinds (`2147483647l_ud1`,
  `'u'da`, `"def"yyy` are invalid); a string with a bad suffix poisons the
  *entire* concatenation sequence (one `invalid`, sources space-joined).
- Integer ladder exactly Table 6 (decimal unsuffixed never goes unsigned;
  out of range → `invalid`). Hexdump width = sizeof chosen type (4/8).
- The reference accepts C99-style hexadecimal floating literals
  (`0x1.8p1`), with only `l`/`L` as a suffix (`0x1p3f` invalid), scanned
  strtod/strtold-style (istringstream cannot parse them); decimal floats
  keep the starter-code istringstream scan for bit-perfect hexdumps.
- A pp-number containing a ud-suffix is validated by the reference's
  scanner, which keeps running its number DFA inside the suffix
  (`numeric_literals.cpp` UdNumberShapeScanner documents the quirks:
  `123_e3` invalid / `123_ex` valid / `123_xe3` valid via hex switch /
  `1e_e3` floating with prefix `1e` / `078_x` invalid but `078e2_x`
  floating / hex fractions need their `p` before a suffix).
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
- String concatenation: >1 distinct encoding prefix or >1 distinct
  ud-suffix → one `invalid`; sources joined with single spaces; array
  length counts code units incl. the appended terminator.
- Raw string bodies decode literally (no escape processing); source
  spelling retains the full `R"delim(...)delim"` form from PA1.
- stderr is not compared; stdout and exit status are.

Differential validation: ~1900 generated cases (numeric shapes, char and
string forms, error paths) byte-compared against `posttoken-ref`,
including exit statuses; regression tests 460/470/480/490 capture the
discovered hex-float, ud-shape, and encoding-edge behavior with refs
regenerated through `make -C pa2 ref-test`.

## Validation

- `make test-pa2` then root `make test-report-through-pa2` (the through
  check is the exit criterion; PA1 must stay green).
- `perl scripts/cppgm_file_audit.pl --stage pa2 --paths dev/src` clean:
  functions ≤ 120 lines, nesting ≤ 6, no stub/shortcut markers.
