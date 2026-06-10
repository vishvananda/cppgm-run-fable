# PA5 preproc — audit

## Audit Plan

Scope: the PA5 commit e5fb8e80f ("Implement PA5 preproc") against
`pa5/plan.md`, the assignment README, and the PA4 baseline (70ec184c3).
Unlike PA4, this commit edits shared PA1-PA4 sources (pp_tokenizer,
pp_token, macro_table, macro_expand, ctrl_expr, post_token), so
regression risk to earlier tools is a primary axis.

Files to inspect:

- `dev/preproc.cpp` — driver; verify it runs the real phase 1-7
  pipeline (Preprocessor -> PostTokenizer -> outfile printer) per
  srcfile with fresh state, asctime captured once at entry, phase-7
  `invalid` tokens rejected, all errors confined to EXIT_FAILURE, and
  no shortcut/replay/interpreter path.
- `dev/src/preprocess.{h,cpp}` — the phase-4 walk; verify against the
  README and 16.1/16.2/16.4/16.6:
  - conditional stack semantics (ordering violations recognized even
    in inactive sections; expressions evaluated only under
    `parent_active && !taken`; extra-token checks gated on
    `parent_active`; groups must close in the file that opened them);
  - `#if`/`#elif` evaluation order: fold well-formed `defined` before
    macro replacement, identifier-like operators accepted as operands,
    empty-expression and `error` outcomes are failures;
  - `#include`: operand macro-replaced, exactly one header-name or
    ordinary string-literal, course two-path search (presumed-__FILE__
    directory then CWD), pragma-once fileid skip before re-reading,
    depth cap, presumed name of the new instance is the chosen path;
  - `#line`: any integer-literal type accepted, offset anchored at the
    directive's terminating new-line, string rewrites presumed name;
  - `#pragma`/`_Pragma`: `once` marks fileid(presumed __FILE__), extra
    tokens after `once` are an error, unknown pragmas ignored;
    `_Pragma` recognized only at text-sequence flush after all macro
    replacement, must head `( string-literal )`, invocation removed,
    destringize per 16.9, concatenation flows across the removal;
  - file/line tracking: tokens stamped at tokenize time from the
    phase-1 byte stream, file instances append-only, directive tokens
    re-stamped to the terminating new-line's line, `__LINE__` =
    physical + mutable per-instance offset.
- `dev/src/pp_tokenizer.cpp` — the line sink; verify the incremental
  new-line count is correct (emission offsets nondecreasing, raw-string
  rescans only skip forward) and that the no-sink path is unchanged for
  PA1-PA4 tools.
- `dev/src/macro_expand.cpp` — builtin invocation and position
  re-stamping; verify builtins respect blue paint and noninvokable,
  produced tokens take the head's file and the invocation's last
  token's line (close paren for function-like), and the PA4 expansion
  semantics are otherwise byte-identical.
- `dev/src/ctrl_expr.{h,cpp}` — the typed result; verify
  `FinishLine`/`EvaluateControllingExpression` produce exactly the
  prior strings (PA3 regression), and PA5 reads truth from the value.
- `dev/frontend_source_sets.mk`, `pa5/Makefile` — preproc links the
  real PA1-PA4 objects plus preprocess; tests run `pa5/tests` and
  `cppgm.tests/course/pa5` against `preproc-ref`.

Cheating-pattern checks:

- No test-specific or source-shape acceptance gates (grep the new and
  changed units for fixture names, input-length switches, canned
  outputs, getenv, argv sniffing beyond the documented flags).
- No fallback success paths: every malformed directive, failed include
  search, bad `#line` operand, bad `_Pragma` shape, `#error`, dangling
  conditional, and phase-7 `invalid` must fail, not degrade.
- No interpreter/VM/trampoline/templated-binary/copied-runtime or
  embedded-payload substitutes; outputs must come from the real
  pipeline. Validate by differential fuzzing against `preproc-ref` on
  generated inputs never used during implementation.
- File-audit conformance: no oversized files, no code hidden in
  unchecked paths (`perl scripts/cppgm_file_audit.pl --stage pa5`).

Ownership / stringly-facts risks to inspect:

- Conditional state must be typed fields on the stack entries, not
  re-derived; activity must be a single predicate (`IsActive`).
- `#if` truth must come from the typed `CtrlExprResult`, not from
  parsing the PA3 output string (the finding that motivated the
  ctrl_expr change).
- Presumed file/line must live in one place (the file-instance table);
  tokens must carry indices, not copied names; `__FILE__`/`__LINE__`
  values must be computed from the instance, not cached in strings.
- Builtin-ness must be a typed `MacroDefinition` field consulted by
  the expander, not a name comparison sprinkled across units.
- Destringize/stringize/file-name-spelling must not duplicate escape
  logic owned elsewhere.

Performance risks to inspect:

- The per-token line scan in the tokenizer must be incremental over
  the file (no per-token rescan from offset 0).
- Include processing must read and tokenize each included file once
  per inclusion, with no repeated directory scans; the pragma-once
  check must precede the re-read.
- The directive walk must be linear in tokens; watch for avoidable
  full-vector copies per directive or per text-sequence flush
  (`TokenizeSource` return, `line`/`args` copies) and for the expander
  re-stamp pass staying O(produced tokens).
- Paint interning behavior must be unchanged from PA4 (no new
  per-token allocations on hot expansion chains).
- Stress: PA4's 2^18 doubling chain through preproc, deep include
  chains near the 256 cap, many sequential includes of one file,
  `__LINE__`/`__FILE__` storms, large line counts, and wide
  conditional nesting — compare time/memory against `preproc-ref`.

## Findings

Cheating-pattern checks — all clean:

- The driver and walk run the real phase 1-7 pipeline end to end:
  TranslateSource + TokenizePPTokens (with the line sink) into the
  directive walk, the PA4 expander, the PA3 evaluator, and one PA2
  PostTokenizer per srcfile. Predefined-macro spellings, destringized
  `_Pragma` payloads, and pastes all retokenize through the same
  pipeline; nothing is hand-assembled or replayed. No interpreter, VM,
  trampoline, templated-binary, copied-runtime, or embedded-payload
  substitute anywhere.
- No test-specific gates: greps over the changed units for fixture
  names, getenv, input-shape switches, and canned outputs come up
  empty; the only argv handling is the documented `-o` contract and
  the `--batch-stdin` guard that *refuses* to run without the test
  runner build.
- File audit passes (34 files); no oversized files or unchecked-path
  fragments.

Behavioral findings (differential fuzzing vs `preproc-ref`: ~10k
grammar-biased cases on fresh seeds with filesystem-backed include
trees — error-biased and valid-biased modes — plus targeted probes):

1. Extra tokens after `#else` were rejected whenever the parent was
   active. The reference accepts `#if 1 ... #else junk #endif` and
   `#if 0 #elif 1 ... #else junk`: the error fires only when the
   `#else` becomes the active branch (`parent_active && !taken`).
   Probes confirmed `#endif`'s rule is unchanged (`parent_active`,
   including over an inactive group: `#if 0 ... #endif junk` errors).
   Eight of twelve first-round fuzz mismatches were this rule.
2. `#pragma once` (and `_Pragma("once")`) with a presumed `__FILE__`
   that cannot be stat'ed — typically after `#line N "name"` rewrote
   it — was silently ignored (a fallback success path, exactly the
   class this audit hunts). The reference exits with an error. The
   remaining four first-round mismatches were this bug.
3. The include depth cap was a guess (256). The reference accepts a
   198-deep include chain (byte-identical output) and fails the 199th
   ("Maximum include nexting reached"); pinned by bisection.

Performance findings (stress vs the reference, fixed not deferred):

4. Avoidable full-vector and per-token copying in the walk and the
   expander: `TokenizeSource` returned the collector's token vector by
   copy; `ProcessFileTokens` copied every token into directive lines
   and text-sequences; `ExpandTextSequence` copied the sequence into
   its deque; `Scan`/`CollectArguments`/`Substitute`/`PastePass`
   copied each flowing token one to three more times. All are moves
   now. A 100k-invocation `__LINE__`+paste storm: 2.76s -> 1.37s (ref
   0.81s); a 100k-directive conditional nest: 0.55s -> 0.21s (ref
   0.20s); an 8MB plain token file: 5.06s -> 2.83s (ref 1.04s);
   PA4's 2^18 doubling chain unchanged at 0.34s (ref 0.39s). Outputs
   byte-identical throughout.
5. Confirmed non-findings: the tokenizer's line counter is one
   incremental scan per file (no per-token rescans); pragma-once
   fileid lookup precedes the include re-read (10k-include and
   pragma-once-storm cases beat the reference: 0.21s vs 0.26s, 0.03s
   vs 0.10s); scaling is linear (0.61/1.36/2.68/5.78s for
   50k/100k/200k/400k-line files). Peak memory is proportional to the
   file (627MB on the adversarial 8MB single-text-sequence file) — the
   materialized phase 1-3 design shared since PA1 (`posttoken` 229MB,
   `macro` 814MB on the same input) versus the reference's ~8MB
   streaming; documented as the established architecture trade in
   `pa5/plan.md`, not deferred work.

Ownership findings — none requiring change: conditional state, file
instances, builtin-ness, and `#if` truth are all typed facts owned at
their decision point (the `CtrlExprResult` and `IBuiltinTokenSource`
designs were built for exactly this). The directive-name dispatch and
the identifier-like-operator predicate are spelling checks at the
layer where spellings are the fact being decided.

## Changes Made

- `dev/src/preprocess.cpp`:
  - `HandleElse`: extra-token error now requires the `#else` to become
    the active branch (`parent_active && !taken`), matching the
    reference; `#endif` keeps `parent_active`.
  - `PragmaOnceCurrentFile`: a failed stat of the presumed `__FILE__`
    throws instead of silently skipping the insert.
  - `kMaxIncludeDepth` 256 -> 198 (reference-pinned by bisection).
  - `TokenizeSource` moves the collector's vector out;
    `ProcessFileTokens` owns its token vector and moves tokens into
    directive lines and text-sequences; `FlushText` moves the
    sequence into the expander.
- `dev/src/preprocess.h`: `ProcessFileTokens` takes the token vector
  by value (every call site passes an rvalue).
- `dev/src/macro_expand.{h,cpp}`: added the rvalue
  `ExpandTextSequence` overload (move into the scan deque); `Scan`,
  `CollectArguments`, `Substitute`, and `PastePass` move tokens
  through the rescan path instead of copying (replacement-list and
  cached-argument tokens are still copied — they are re-read).
  Expansion semantics intentionally byte-identical; PA4's lvalue call
  sites are untouched.
- `pa5/plan.md`: corrected the `#else` extra-token rule, the
  pragma-once stat rule, and the include depth cap; added Architecture
  Review and Final Architecture Review.

## Validation

- `make test-report-through-pa5`: 240/240 (PA1-PA5, all 5 stages)
  after every change — required exit criterion.
- `perl scripts/cppgm_file_audit.pl --stage pa5 --paths dev/src`:
  pass (34 files) — required exit criterion.
- Differential fuzzing vs `preproc-ref`: generator covering
  conditionals (well- and mal-formed orderings), includes over a real
  header tree (pathrel vs CWD, header-name vs string vs macro-built
  operands, missing files, mutual recursion with guards/pragma once),
  `#line` rewrites feeding includes and pragma-once, `_Pragma`
  (well- and mal-formed), object/function/variadic macros with
  `#`/`##`, `__FILE__`/`__LINE__` in text/arguments/directives, and
  1-2 srcfiles per case. First round (seeds 11/22/33/44, 1600 cases):
  12 mismatches, all explained by findings 1-2. After the fixes:
  seeds 101-606 (4200 error-biased) and 711-766 (4200 valid-biased)
  plus a 3000-case re-run after the move refactor — zero mismatches;
  every both-success case byte-identical, every failure mutual.
- Reference probe battery (re-run after all changes, all MATCH):
  `#else`/`#endif` extra-token matrix across
  active/taken/elif-taken/inactive-parent/second-`#else` states,
  pragma-once stat failures via `#line`, `#line 0`/`16u`, spliced
  `#if __LINE__`, identifier-like operators after `#ifdef`/`defined`,
  `_Pragma` concatenation and `u8` string payloads, multi-line
  invocation `__LINE__`, includes under `#if 0`, `#pragma once extra`,
  empty `#if`, dangling `#elif`/`#endif`.
- Include depth: 198-chain passes byte-identical on both binaries;
  199-chain fails on both.
- Stress (all outputs byte-identical to the reference): doubling
  chain 0.34s/26MB (ref 0.39s/8.2MB); 10k sequential includes
  0.21s (ref 0.26s); pragma-once storm 0.03s (ref 0.10s);
  100k-directive nest 0.21s (ref 0.20s); `__LINE__`/paste storm
  1.37s (ref 0.81s); 8MB token file 2.83s (ref 1.04s) with linear
  scaling across 50k-400k-line variants.
