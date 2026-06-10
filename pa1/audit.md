# PA1 (pptoken) Audit

## Audit Plan

Scope: the PA1 implementation commit f244638c7 ("Implement PA1 pptoken:
translation phases 1-3 and pp tokenizer") against `pa1/README.md`,
`pa1/plan.md`, and the through-pa1 test suites.

Files to inspect:

- `dev/pptoken.cpp` — tool entry point: real work only, no dummy or
  fallback success paths, correct failure mapping.
- `dev/src/utf8.{h,cpp}` — decode strictness vs. the documented
  Windows-1252 fallback; encoder bounds; agreement with `pptoken-ref`.
- `dev/src/source_translation.{h,cpp}` — phase 1-2 ordering, trigraph /
  splice / UCN lookahead-consumption semantics, BOM and final-LF handling,
  raw-to-translated source mapping integrity.
- `dev/src/pp_tokenizer.{h,cpp}` — phase 3 scanner: all eleven token
  kinds, header-name context tracking, `<::` disambiguation, encoding
  prefixes, ud-suffixes, raw-string raw-mode rescan, error paths.
- `dev/frontend_source_sets.mk`, `dev/Makefile`, `pa1/Makefile` — build
  registration; confirm the tested binary runs the real tool `main`
  (test-runner wrapping renames it to `test_runner_real_main` and forks a
  child per request — no substitution of compiler work).
- Handout skeleton files consumed by the new code
  (`IPPTokenStream.h`, `DebugPPTokenStream.h`, `test_runner.cpp`) —
  confirm unmodified and used as intended.

Checks to perform:

1. Cheating/shortcut sweep: no test-name or source-shape gates, no
   shelling out to `pptoken-ref`, no embedded fixture payloads, no
   fallback "emit something plausible and exit 0" paths, no skipped
   phases.
2. Differential probes against `pptoken-ref` for every behavior
   `pa1/plan.md` claims was established empirically: Windows-1252 stray
   bytes, overlong/surrogate sequences, encoder bound (`\U0010FFFF`),
   `\UFFFFFFFF` in-band EOI, trigraph/`?`-lookahead consumption,
   post-splice raw read, hex pp-number `p`-exponent vs the README's
   `e sign` grammar, raw-string delimiter laxness, header-name and
   comment error paths.
3. Performance: scan for quadratic behavior (tokenizer lookahead, raw
   string close scan, operator munch), per-character allocation, repeated
   full-stream walks; measure throughput on a large generated input.
4. Ownership boundaries: source mapping as the single owner of
   raw-vs-translated facts; no stringly re-derivation of facts the
   translator already computed; header-name context owned by the
   tokenizer state machine, not by output inspection.
5. File-audit integrity: `perl scripts/cppgm_file_audit.pl --stage pa1
   --paths dev/src` passes with no exemption tricks, no code moved to
   unchecked paths, no hidden implementation fragments under `pa1/`.
6. Regression scope: PA1 is the first stage; confirm
   `make test-report-through-pa1` covers both `pa1/tests` (28 inputs) and
   `cppgm.tests/course/pa1` (21 inputs) and that all other tools still
   build.

## Findings

1. **Phase interleaving defect (fixed).** The implementation ran phase 1
   eagerly (whole-file UTF-8 decode into a code point vector) and phase 2
   over decoded code points. The reference interleaves the phases at the
   byte level: lookahead consumed after a backslash, a lone `?`, or a
   splice is read as one raw byte identity-mapped to a code point (byte
   0x93 becomes U+0093, not Windows-1252 U+201C; a multi-byte sequence is
   split, leaving its continuation bytes to fail normal decoding). A
   3000-case differential fuzz surfaced 47 inputs my build accepted that
   `pptoken-ref` rejects (e.g. `\é`, including inside comments), plus
   silent token-spelling differences (`\<0x93>`, `?<0x93>`,
   post-splice strays) and rejections of inputs the reference accepts
   (`\<0xA3>`, `\<0xFF>`, `\<0xC1>` — the consumed byte is a legal
   identity code point).
2. **Eager decode errors were also too early (fixed).** A malformed byte
   is ill-formed only if tokenization reads it in translated mode. The
   reference accepts files whose bad bytes sit inside raw string literals
   (`R"(\é)"`) or after the in-band `\UFFFFFFFF` end-of-input sentinel;
   the eager decoder threw at translation time for the whole file.
3. **Leading-BOM consumption rule (fixed).** The reference drops a leading
   BOM and consumes the following character without phase-2
   interpretation: `<BOM>??=` lexes as `?` `?` `=`, and `<BOM>\` is an
   inert backslash (no splice, no UCN). The byte after it is decoded
   normally (multi-byte and Windows-1252 fallback apply). The original
   byte-erase approach let the next character start a trigraph.
4. **Raw-string delimiter semantics confirmed, spelled out.** Probing
   showed the closing `)delim"` is matched by decoded code point, not
   byte-wise (a stray 0x93 delimiter closes against its 3-byte UTF-8
   spelling), the 16-char limit counts decoded characters (9 two-byte
   chars pass), and content/delimiters use normal decoding (strays in
   0xA0-0xBF are errors even inside raw strings). The rewrite preserves
   all of this; `pa1/plan.md` now documents it.
5. **Stale plan claims (fixed).** `pa1/plan.md` claimed "strict UTF-8
   decoding (rejects overlong forms, surrogates...)" while the code (and
   the reference, verified by probe: overlong `0xC0 0x80` and surrogate
   `0xED 0xA0 0x80` decode and re-encode) does no range validation.
   Plan text corrected.
6. **No cheating or shortcut paths found.** No test-name or source-shape
   gates, no reference-binary invocation from the tool, no embedded
   fixture payloads, no fallback success paths. The test-runner wrapping
   (`-Dmain=test_runner_real_main` + fork-per-request worker) is
   pre-existing harness infrastructure that executes the real tool main.
7. **No performance blockers.** All passes are linear; the operator munch
   is bounded at 4 chars, delimiter match at 17 decoded chars per `)`
   candidate. Measured 2 MB realistic source at ~0.4 s CPU and a 2 MB
   adversarial all-`)` raw string at ~0.1 s. The 24-byte-per-char source
   map is transient and negligible at course input sizes.
8. **File-audit integrity.** `cppgm_file_audit.pl --stage pa1` passes on
   the rewritten sources; no exemptions, no code moved to unchecked
   paths. The `using std::string` in `pp_tokenizer.h` exists to satisfy
   the unqualified names in the handout skeleton headers
   (`IPPTokenStream.h`), which are intentionally unmodified.

## Changes Made

- `dev/src/utf8.{h,cpp}`: replaced the whole-buffer `DecodeUtf8` /
  `EncodeUtf8Range` API with single-character `TryDecodeUtf8Char` (returns
  false on malformed bytes, advancing one byte) and `DecodeUtf8Char`
  (throwing), plus `ReencodeUtf8Range` for raw-string output. Encoder
  unchanged (rejects negatives and >= U+10FFFF, the verified reference
  bound).
- `dev/src/source_translation.{h,cpp}`: `TranslatedSource.raw` (code
  points) replaced by `TranslatedSource.bytes`; the translator now walks
  bytes, interleaving decode with trigraph/splice/UCN handling; consumed
  lookahead is emitted via `EmitByte` (identity mapping); decode failures
  emit in-band `kInvalidChar`; leading-BOM rule implemented in `Run()`;
  `kEndOfInputChar`/`kInvalidChar` constants now owned by this header.
- `dev/src/pp_tokenizer.cpp`: all translated-stream reads funnel through
  `At()`, which throws when a `kInvalidChar` is read in translated mode
  (comment skippers and header-name scans included, since the reference
  rejects bad bytes inside comments); raw-string scanning rewritten over
  `bytes` with decoded-character delimiter matching and on-demand
  re-encoding; resume logic unchanged (byte offsets, same binary search).
- `pa1/plan.md`: corrected the utf8 strictness claim, documented the
  byte-interleaved pipeline, BOM consumption, raw-string decoded-character
  matching; added Architecture Review and Final Architecture Review.

## Validation

- `make test-report-through-pa1`: 49/49 pass.
- `perl scripts/cppgm_file_audit.pl --stage pa1 --paths dev/src`: pass
  (12 files).
- ~70 targeted probes vs `pptoken-ref` (UTF-8 strictness, Windows-1252
  fallback, identity-byte consumption, BOM rules, UCN edge cases,
  trigraph/splice lookahead, raw-string delimiters and content, header
  names, digraphs, `<::`, pp-number `p`-exponent, escapes, ud-suffixes):
  all byte-exact on success, status-matched on failure.
- Differential fuzzing, byte-exact comparison: 3000 cases (seed 20260610,
  general alphabet), 4000 cases (seed 987654321, stray/multi-byte heavy),
  5000 cases (seed 31337, combined): 0 diffs after the fix (47 before).
- Self-tokenization: `pptoken` output byte-identical to `pptoken-ref` on
  all five implementation source files.
- Performance: 2 MB realistic source ~0.4 s CPU; 2 MB adversarial raw
  string ~0.1 s; linear scaling, no timeout risk.
