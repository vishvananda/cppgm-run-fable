# PA4 macro — audit

## Audit Plan

Scope: the PA4 commit 4ffe694d1 ("Implement PA4 macro") against
`pa4/plan.md`, the assignment README, and the PA3 baseline (03f01a822).

Files to inspect:

- `dev/macro.cpp` — driver; verify it runs the real phase 1-3 pipeline
  (`TranslateSource` + `TokenizePPTokens`) into the real phase 4-7
  pipeline (collector -> preprocessor -> PostTokenizer), with no
  shortcut path, no interpreter/VM/replay substitute, and errors
  confined to `EXIT_FAILURE`.
- `dev/src/pp_token.{h,cpp}` — the typed token value; verify whitespace
  folding matches the directive grammar (whitespace never becomes a
  token, new-lines stay tokens), `EmitPPToken` replays every kind
  losslessly, and the digraph helpers cover exactly `%:`/`%:%:`
  (there are no digraph spellings for `(`/`)`/`,`).
- `dev/src/macro_table.{h,cpp}` — definition-side semantics; verify the
  directive grammar in the README is enforced exactly (no-whitespace
  lparen, must-whitespace object-like, identifier-list shape,
  `...` last, `#`/`##` placement, `__VA_ARGS__` placement, `#undef`
  trailing tokens) and that 16.3p1 redefinition identity uses spelling
  sequence plus whitespace *presence*, not raw spacing strings.
- `dev/src/macro_expand.{h,cpp}` — the rescan engine; verify against
  the README traces and 16.3:
  - invocation recognition (function-like needs `(`, possibly produced
    by earlier replacement; abort flagged noninvokable identifiers);
  - argument collection (raw, paren depth, commas at depth 0 only,
    variadic folding, 16.3p4 count checks, `()` for zero parameters);
  - substitution order (stringize from raw, paste operands raw with
    placemarkers, ordinary parameters from cached full expansion);
  - blue paint: replacement-origin tokens get (head ∩ close) ∪ {M},
    argument-origin tokens keep own paint plus full head paint ∪ {M}
    (the course deviation pinned by 910-recurse2 / 700-redef-q);
  - paste retokenization through the real phase 1-3 pipeline, exactly
    one resulting token, no string-level token fabrication;
  - whitespace propagation rules used by later stringize.
- `dev/src/macro_preprocess.{h,cpp}` — phase-4 walk; verify directive
  recognition matches the four README prefixes, text-sequences are
  maximal, new-lines inside text become whitespace, the table state at
  each text-sequence is the state at that point, one PostTokenizer
  instance spans the file (phase-6 concatenation across directives),
  and non-define/undef directives fail rather than being skipped.
- `dev/frontend_source_sets.mk`, `pa4/Makefile` — macro links the real
  PA1/PA2 objects plus the four new units; tests run both `pa4/tests`
  and `cppgm.tests/course/pa4`.
- Shared PA1-PA3 sources — the PA4 commit must not change their
  behavior (regression risk for `make test-report-through-pa1/2/3`);
  confirm via the commit diff and the full report.

Cheating-pattern checks:

- No test-specific or source-shape acceptance gates (search the macro
  units for fixture names, input-length switches, canned outputs).
- No fallback success paths: every malformed directive, argument-count
  violation, bad paste, and misplaced `__VA_ARGS__` must throw, not
  degrade to echoing tokens.
- No embedded reference payloads, output replay, or copied runtime;
  validate behavior by differential fuzzing against `macro-ref` on
  generated inputs never used during implementation, including the two
  documented deliberate divergences (16.3p4 strictness) to confirm the
  divergence list in `pa4/plan.md` is accurate and complete.
- File-audit conformance: no oversized files, no code hidden in
  unchecked paths (`perl scripts/cppgm_file_audit.pl --stage pa4`).

Ownership / stringly-facts risks to inspect:

- Macro kind, variadic-ness, and parameter slots must be typed fields
  on `MacroDefinition`, not re-derived from spellings downstream.
- `#`/`##` operate-vs-inert must be a token fact (`paste_op`) decided
  at definition validation, not re-inferred during substitution.
- Blue paint must live on tokens (per the course design), not in
  expander-global state; `noninvokable` must be sticky.
- Whitespace must be a token fact (`ws_before`) folded once in the
  collector, not recomputed by scanning spellings.
- The same `__VA_ARGS__`/digraph spelling constants must not be
  duplicated as ad-hoc literals across units.

Performance risks to inspect:

- `Scan` re-inserts replacement tokens at the deque front — confirm
  total work is bounded by tokens produced, not quadratic rescans.
- Argument re-expansion: each parameter's full expansion must be
  computed at most once per invocation (memoized), even when the
  parameter appears many times in the replacement list.
- Redefinition identity and `ParamIndex` are linear in definition size
  — confirm no hidden full-table or full-file scans per token.
- Per-token `set<string>` blacklists — confirm copies are proportional
  to paint actually carried and no pathological growth on the course
  suite (paint sets are bounded by active macro nesting depth).

## Findings

Cheating-pattern checks — all clean:

- The PA4 commit (4ffe694d1) touches only the four new units, the
  driver, `dev/frontend_source_sets.mk`, and `pa4/plan.md`; no shared
  PA1-PA3 source changed, so no regression vector exists by
  construction (and the full report passes).
- No test-specific gates, hidden I/O, env switches, or canned outputs in
  any macro unit (grep for fixture names, `getenv`, file access, argv
  inspection: nothing). The driver runs the real phase 1-3 pipeline into
  the real phase 4 walk into the real PA2 PostTokenizer; pastes re-lex
  through `TranslateSource` + `TokenizePPTokens`; every error path
  throws and `main` maps it to EXIT_FAILURE. No interpreter, VM,
  replay, or embedded-payload substitute anywhere.
- The README's worked examples (f(f(x)) trace, z[0] sticky
  noninvokable, g(f)(g)(3), f(g)b) rescan) were re-walked against the
  code by hand and against the binary; all 68 pa4 fixtures pass.

Behavioral findings (differential fuzzing vs `macro-ref`, ~12k
grammar-biased cases on fresh seeds, plus targeted probes):

1. The reference enforces 16.3.2p1 ("# shall be followed by a
   parameter") only for macros with at least one parameter; for
   `#define M() # b` it stringizes whatever follows. gcc/clang reject;
   no fixture covers it; mine keeps EXIT_FAILURE. `pa4/plan.md` claimed
   all probed `#` edges matched the reference — that claim was wrong
   and is now corrected with this entry in the divergence list.
2. The reference consumes directive-pattern lines inside an open
   argument list as argument tokens; the handout says those patterns
   "must start a preprocessing directive" and 16.3p11 makes them UB
   inside invocations. Mine reports the resulting unterminated
   invocation. Documented as a divergence.
3. After an invocation spanning a new-line, the reference re-enters
   directive recognition mid-rescan: a `#`/`%:` pp-token emitted later
   by the same text-sequence makes it fail where 16.3p10 (new-line is
   ordinary whitespace in an invocation) says the token is just an
   `invalid` posttoken. Mine follows the standard and succeeds.
   Documented as a divergence.
4. PA2 layer: both `posttoken-ref` and `macro-ref` accept `q`/`Q` as a
   long-double floating suffix (GCC `__float128` flavor), e.g.
   `3.5 ## Q` pastes to `3.5Q` which the reference types as long
   double. C++11 2.14.4 lists `f l F L` only; mine reports `invalid`.
   Documented in `pa2/plan.md`.
5. The two 16.3p4 argument-count divergences documented at
   implementation time were re-confirmed (including that the reference
   rejects too-few arguments just like mine — the leniency is only
   excess-fold and missing-variadic-part).

Performance and ownership findings — fixed, not deferred:

6. Per-token `set<string>` blue paint made token copies O(paint) with
   one allocation per name per copy, and value-identical sets were
   allocated per invocation. A 2^18 doubling chain took 8.21s/419MB vs
   the reference's 0.39s/8.3MB. Fixed by interning (finding-by-design:
   the chain has only ~19 distinct paint sets).
7. `ParamIndex` re-derived parameter slots from spellings on every
   substitution — downstream recovery of a definition-time fact. Fixed
   by stamping `param_index` during definition validation.
8. `PastePass` ran a full copy pass over every substitution result even
   for paste-free replacement lists. Fixed with a definition-time
   `has_paste` flag (placemarkers can only arise next to a paste).
9. The driver flushed stdout per token (`endl`), ~260k write syscalls
   on the doubling chain. Fixed: `"\n"` + exit flush.
10. The `__VA_ARGS__` spelling constant was defined separately in
    `macro_table.cpp` and `macro_expand.cpp` (duplicated ownership of
    one fact). Consolidated as `kMacroVaArgs` owned by `macro_table`.

## Changes Made

- `dev/src/pp_token.h/.cpp`: `blacklist` is now an interned `PaintSet`
  (`shared_ptr<const vector<string>>`, null = empty, sorted-unique
  invariant); added `PaintContains` and `PaintInterner`
  (insert/union/intersect, hash-consed via a by-value map plus
  by-pointer memo maps so repeated derivations are O(log) with pointer
  keys). Added `param_index` to `PPToken` (definition-time parameter
  slot, -1 otherwise).
- `dev/src/macro_table.h/.cpp`: exported `kMacroVaArgs`; added
  `MacroDefinition::has_paste`; validation now stamps `param_index` on
  every replacement-list token (`ParamSlot`, moved from macro_expand)
  and uses it for the `#`-followed-by-parameter check.
- `dev/src/macro_expand.h/.cpp`: dropped `ParamIndex`/`IntersectPaint`/
  `AddPaint` and the local `kVaArgs`; painting goes through the
  expander-owned `PaintInterner` (`base`, `arg_paint`, paste results);
  blacklist membership via `PaintContains`; the paste pass runs only
  when `macro.has_paste`. The 16.3/course semantics are intentionally
  byte-identical.
- `dev/macro.cpp`: per-token `endl` replaced with `"\n"`.
- `pa4/plan.md`: divergence list corrected and extended (zero-parameter
  `#`, directives inside argument lists, line-spanning invocation
  directive re-entry, PA2 `q`/`Q` suffix); architecture section updated
  for `PaintSet`/`param_index`/`has_paste`; added Architecture Review
  and Final Architecture Review.
- `pa2/plan.md`: documented the reference's `q`/`Q` floating-suffix
  extension as a deliberate divergence.

## Validation

- `make -C pa4 test`: 38/38 local + 30/30 course fixtures pass after
  every change.
- `make test-report-through-pa4`: 172/172 (PA1-PA4) — required exit
  criterion.
- `perl scripts/cppgm_file_audit.pl --stage pa4 --paths dev/src`: pass
  (32 files).
- Differential fuzzing vs `macro-ref`: grammar-biased generator
  (directives, undefs, redefinitions, nested invocations, `#`/`##`,
  digraphs, raw strings, multi-line argument lists), seeds
  20260610/7/99/12345/424242/555 × 1500-2500 cases each, before and
  after the refactor. Every non-identical case falls into the
  documented divergence buckets (my EXIT_FAILURE with the specific
  arg-count/`#`-placement messages, or the reference's two quirks where
  mine succeeds); zero unexplained mismatches, and all both-success
  cases byte-identical.
- Stress: 2^18 doubling chain 8.21s/419MB -> 0.35s/22MB (reference:
  0.39s/8.3MB), 20k-invocation paste+stringize file 0.60s -> 0.33s
  (reference: 0.33s); outputs byte-identical to the reference in both.
