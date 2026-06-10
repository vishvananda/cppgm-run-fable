# PA5 preproc — design plan

## Goal

`preproc -o <outfile> <srcfile1> ... <srcfileN>` runs translation phases
1-6 and the tokenization part of phase 7 over each source file in turn
and writes `preproc <N>` followed by one `sof <srcfile>` + PA2 posttoken
section + `eof` per file. Each srcfile is processed with completely
fresh preprocessor state (macro table, conditional stack, pragma-once
set, file/line tracking); only the `__DATE__`/`__TIME__` strings
(asctime, captured once at main entry) are shared. Any error exits
EXIT_FAILURE; outfile contents are only compared on success.

PA5 adds to the PA4 walk: conditional inclusion (`#if`/`#ifdef`/
`#ifndef`/`#elif`/`#else`/`#endif` over the PA3 evaluator), `#include`
with the course-defined two-path search, `#line`, `#error`, `#pragma`
(`once` + ignored unknowns), null directives and non-directives, the
predefined macros, the `_Pragma` operator, and source file/line
tracking for `__FILE__`/`__LINE__`. A phase-7 `invalid` token is now an
error instead of output.

## Architecture / ownership

- `dev/src/pp_token.h` (extended): `PPToken` gains the physical
  position macro replacement and the preprocessor share: `line` (1-based
  physical source line of the token's first character) and `file` (index
  into the active `Preprocessor`'s file-instance table; -1 outside PA5).
  `PPTokenCollector` implements the new `IPPTokenLineSink` and stamps
  `line` on each collected token; `file` is stamped by the preprocessor
  when it loads a file. Earlier tools pass no sink and are untouched.
- `dev/src/pp_tokenizer.{h,cpp}` (extended): `TokenizePPTokens` takes an
  optional `IPPTokenLineSink*`. The tokenizer counts new-line bytes in
  the phase-1 byte stream up to each token's first source byte
  (emission offsets are nondecreasing, so the count is incremental) and
  reports the line before every token emission. Lines are a phase-1/2
  fact (line splices and block comments hide new-lines from phase 3),
  which is why the tokenizer, not the collector, must compute them.
- `dev/src/macro_table.{h,cpp}` (extended): `MacroDefinition` gains a
  `builtin` kind (`kBuiltinNone`/`kBuiltinFile`/`kBuiltinLine`) and
  `MacroTable::DefineBuiltin` registers `__FILE__`/`__LINE__` so
  `#ifdef`/`defined` see them through the ordinary `Lookup`. All other
  predefined macros are plain object-like definitions installed by the
  preprocessor at srcfile start (spellings tokenized through the real
  phase 1-3 pipeline, not hand-built tokens).
- `dev/src/macro_expand.{h,cpp}` (extended):
  - `MacroExpander` takes an optional `IBuiltinTokenSource*`. Invoking a
    builtin macro asks the source for the produced token (a
    string-literal for `__FILE__`, a pp-number for `__LINE__`, computed
    from the head token's own file/line stamp) and paints/places it like
    an object-like expansion result.
  - After every invocation, all produced tokens are re-stamped with the
    head token's file and the invocation's *last* token's line (the
    closing paren for a function-like invocation — reference-pinned:
    the scan position at expansion time, so a multi-line argument list
    reports the rparen's line). Argument tokens were already fully
    expanded with their own stamps first, so `__LINE__` as an argument
    reports the argument's line while `__LINE__` in a replacement list
    reports the invocation site (600/610-line-macro).
- `dev/src/ctrl_expr.{h,cpp}` (extended):
  - `IsDefinedFn` becomes `std::function<bool(const string&)>` so the
    preprocessor can close over its macro table (PA3's mock function
    pointer still converts).
  - The evaluation result becomes typed: `CtrlExprResult { kind ∈
    {none,error,value}, value, is_unsigned }`. `FinishLine` formats the
    PA3 output string from it; new `EvaluateControllingResult` exposes
    it so PA5 reads truth as `value != 0` instead of parsing the
    formatted string. PA3 stream behavior is unchanged.
- `dev/src/preprocess.{h,cpp}` (new): class `Preprocessor` — the PA5
  phase-4 walk. Owns `MacroTable`, `MacroExpander`, the conditional
  stack, the file-instance table, the pragma-once fileid set, and the
  include depth counter; emits replaced text into a caller-supplied
  `IPPTokenStream` (the PA2 `PostTokenizer`, one instance per srcfile so
  phase-6 string concatenation spans directive, include, and inactive-
  section boundaries — pinned by 800-pragma-once and ref probes).
- `dev/preproc.cpp` (rewritten tool entry): argument parsing, asctime
  capture, the outfile stream, `preproc N`/`sof` framing, a printing
  `IPostTokenStream` that throws on `PTK_INVALID` (phase-7 invalid
  tokens are PA5 errors), and the per-srcfile `Preprocessor` +
  `PostTokenizer` wiring. Keeps the starter `PA5FileId`/`PA5GetFileId`
  (stat via syscall) used by the preprocessor for include resolution
  and pragma-once identity.

## File/line tracking

- Each token carries its physical line (stamped at tokenize time) and
  its file-instance index. A file instance (one per `#include`
  occurrence plus one for the srcfile) holds the mutable presumed state:
  `presumed_name` (initially the resolved path; `#line` with a string
  rewrites it) and `line_offset` (presumed = physical + offset;
  `#line N` terminated by a new-line on physical line E sets
  `offset = N - (E + 1)`). Instances are append-only so tokens stored in
  replacement lists stay valid across include exit.
- `__LINE__`/`__FILE__` invocation reads the head token's stamp plus its
  instance's current presumed state. Because text-sequences never span
  directives, every `#line` that should affect a token has already been
  processed when the token's sequence is flushed.
- A directive's tokens are re-stamped with the line of the directive's
  terminating new-line before any expansion (reference-pinned: a
  spliced `#if __LINE__ == \` + `2` evaluates `__LINE__` as 2, the
  directive's end line).

## Directive walk

Same structure as PA4 (new-line-separated, `#` first on a logical
line, directives delimit text-sequences) with a conditional stack and
an include stack:

- Conditional groups: `{parent_active, taken, seen_else, active}`.
  Ordering violations (`#elif` after `#else`, multiple `#else`,
  `#endif`/`#elif`/`#else` without a group in the same file) are
  errors even in inactive sections (170-nondir5, ref probes). Extra
  tokens after `#else`/`#endif` are errors only when `parent_active`
  (ref: "Illegal token after #endif"; ignored inside an inactive
  parent, 170-nondir4). `#if`/`#elif` expressions are evaluated only
  when they can decide (`parent_active && !taken`); otherwise their
  tokens are skipped entirely (garbage allowed, exclude.t).
- Inactive sections process only the conditional directives; all other
  directives and text are skipped (bad `#define`s, bad invocations,
  missing includes are all fine — exclude.t).
- `#ifdef`/`#ifndef`: name may be an identifier or an identifier-like
  operator (`and`, `or_eq`, ... — ref probe); extra tokens after the
  name are ignored (ref probe); a missing or other-kind name is an
  error.
- `#if`/`#elif` evaluation: fold well-formed `defined X` /
  `defined ( X )` (operand identifier or identifier-like operator) to
  pp-number 1/0 *before* macro replacement (16.1p4); macro-replace with
  the PA4 expander; convert tokens to PostTokens in the PA3
  identifier_or_keyword context; evaluate with the PA3 calculator
  (malformed `defined` falls through to the calculator's pinned error
  semantics — outcome is "error" either way, so pre-folding only the
  well-formed shapes is observationally exact). Empty expression
  (before or after replacement) and an "error" result are EXIT_FAILURE.
- `#include` (active only): macro-replace the operand; exactly one
  header-name or one ordinary string-literal token (post-tokenized for
  its UTF-8 value) is accepted. Search `pathrel` (presumed `__FILE__`
  directory + nextf, when `__FILE__` has a `/`) then `nextf` against
  the CWD, testing existence with `PA5GetFileId`. The chosen path
  becomes the new instance's presumed name. A fileid in the pragma-once
  set skips the include. Depth is capped (256; the ref also caps:
  "Maximum include nexting reached"). At end of an included file (and
  the srcfile) the conditional stack must be back at its entry depth.
- `#line` (active only): macro-replace; accept `integer-literal` or
  `integer-literal string-literal` (any integer type — ref accepts
  `#line 16u` and `#line 0`); anything else is an error. Updates the
  current instance's offset (anchored at the directive's terminating
  new-line) and presumed name.
- `#pragma` (active only): `once` (extra tokens are an error — ref)
  adds fileid(presumed `__FILE__`) to the set; everything else,
  including `cppgm_mock_unknown` and an empty pragma, is ignored.
- `#error` (active only): EXIT_FAILURE.
- null directive: ignored. Non-directive: error when active, ignored
  when inactive.
- `#define`/`#undef` (active only): PA4 `MacroTable` unchanged.

## _Pragma operator

Recognized in a replaced text-sequence after all macro expansion, at
flush time: every occurrence of identifier `_Pragma` must be followed
by `(` string-literal `)` or it is an error. The invocation tokens are
removed from the sequence; the string is destringized (prefix dropped,
`\"`/`\\` unescaped; raw strings take their raw content), retokenized,
and dispatched through the same `#pragma` handler. String concatenation
joins across the removed tokens (ref probe: `"a" _Pragma("junk") "b"`
concatenates).

## Predefined macros

`__CPPGM__ 201303L`, `__cplusplus 201103L`, `__STDC_HOSTED__ 1`,
`__CPPGM_AUTHOR__ "Vishvananda Abrams"`, `__DATE__`/`__TIME__` from one
asctime call at main entry, `__FILE__`/`__LINE__` as builtins. Tests
gate only definedness and the fixed values' use in `#if`.

## Validation

- `make -C pa5 test` for the assignment suites (tests/ and course/pa5).
- `make test-report-through-pa5` as the exit criterion (PA1-PA4 must
  stay clean: the PA4 `macro` tool keeps its own restricted walk, and
  the shared pp_token/expander/ctrl_expr changes are extensions that
  default to prior behavior).
- `perl scripts/cppgm_file_audit.pl --stage pa5 --paths dev/src`.
- Reference probes (pinned above) cover the underspecified corners:
  extra-token rules, `#line 0`/`16u`, identifier-like-operator
  operands, unknown pragmas, include depth, concatenation across
  removed `_Pragma`s and inactive sections.
