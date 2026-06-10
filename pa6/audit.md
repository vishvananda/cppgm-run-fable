# PA6 recog — audit

## Audit Plan

Scope: commit 59ac1a99e (PA6 recog) against `pa6/README.md`,
`pa6/pa6.gram`, and `pa6/plan.md`; shared-code changes that touch
PA1-PA5 (`dev/preproc.cpp`, `dev/src/predefined_macros.*`,
`dev/frontend_source_sets.mk`, `dev/src/test_runner.cpp`).

Files to inspect:

- `dev/recog.cpp` — driver: per-file BAD isolation, no fallback-OK
  paths, worker-thread stack strategy, batch-stdin wiring.
- `dev/src/parse/parse_token.{h,cpp}` — terminal conversion: OP_RSHIFT
  split, literal folding, PTK_INVALID rejection, ST_EOF guarantee.
- `dev/src/parse/parse_tree.{h,cpp}` — tree printer (indent clamp;
  output is ungated, so only performance matters).
- `dev/src/parse/parser.h`, `parser_core.cpp` — save/restore
  discipline, bracket-context stack soundness, Peek clamp.
- `dev/src/parse/parse_names.cpp` — mock name lookup gates exactly
  where the grammar names `*-name`; no extra gating.
- `dev/src/parse/parse_expr.cpp` — binary ladder vs grammar operator
  productions, 14.2.3 refusals for `>`/`>>`, sizeof/typeid orderings.
- `dev/src/parse/parse_stmt.cpp` — 6.8 declaration-before-expression,
  condition ordering.
- `dev/src/parse/parse_decl.cpp` — decl-specifier-seq type-name rule,
  specifier-sequence sharing, attribute/balanced-token (ST_NONPAREN).
- `dev/src/parse/parse_declarator.cpp` — 8.2 orderings and FOLLOW
  validation sets against the grammar explorer's FOLLOW pages.
- `dev/src/parse/parse_class.cpp` — pure-specifier (ST_ZERO),
  virt-specifiers (ST_OVERRIDE/ST_FINAL), member-declarator FOLLOW.

Cheating checks:

- No test-specific or source-shape acceptance gates (grep for fixture
  names, srcfile-path probes, token-count shortcuts).
- No fallback success path: every OK must come from a completed
  phase 1-7 pipeline plus a full `translation-unit` parse reaching
  ST_EOF; no "assume OK on error" branch.
- No embedded reference output, no shelling to `recog-ref`, no
  parse-tree payloads substituting for parsing.
- Grammar coverage: every `pa6.gram` nonterminal reachable from
  `translation-unit` has a real parse implementation (cross-check
  nonterminal list against parser.h members); no production silently
  dropped or over-accepted to dodge hard cases.

Regression checks:

- `preproc` predefined-macro move is pure code motion (diff PA5
  behavior; `make test-report-through-pa6` covers PA1-PA5 suites).
- `test_runner.cpp` batch path for recog runs the same pipeline as the
  standalone tool.

Performance risks:

- Backtracking blowup: ordered-choice retries on shared prefixes
  (declaration/expression, type-id/expression, abstract/named
  parameter) — verify no exponential nest (stress deep nesting).
- Per-token allocation and tree construction on failing alternatives.
- Peek clamp arithmetic on every token access; tree printer
  indentation cost on deep trees.
- Per-srcfile thread spawn cost (one thread per file, acceptable).

Ownership boundaries:

- Parse tokens own the spelling-derived facts (mock category,
  override/final, empty-string/zero) — check they are computed once,
  not re-derived stringly at use sites.
- Bracket-context stack owned by the parser core; verify no parse
  function manipulates it directly outside MatchOpenAngle /
  ParseCloseAngleBracket / Advance / Restore.
- Predefined macros shared by preproc and recog from one definition.

File-audit issues:

- `perl scripts/cppgm_file_audit.pl --stage pa6 --paths dev/src` must
  pass; verify no budget meta-files or exclusions were added to dodge
  it, and the parser.h [bad-division] note in plan.md is a warning,
  not a failure.

## Findings

Blockers (fixed in this pass):

1. Exponential backtracking on unparseable nested input. Every
   ordered-choice ambiguity retry (template-argument's three readings,
   sizeof/typeid operands, statement-vs-declaration, abstract-vs-named
   parameter declarators) re-descended the same failing subtree, so
   cost multiplied per nesting level: an 8-deep failing
   `TC1<...%...>` nest took 9.85s (~3^depth) where `recog-ref` took
   0.13s; depth 12 would take hours. A template-heavy file with one
   typo could hang the tool, and the harness batch timeout (10s) was
   one nesting level away — a latent timeout trap, not an algorithmic
   cost the assignment requires.
2. Quadratic re-scan after the exponential was fixed: each level's
   constant-expression retry walked the remaining `TC1 < TC1 < ...`
   chain as a relational expression (the `<` after a template-name was
   allowed as less-than), leaving a failing 1000-deep nest at 1.6s vs
   the reference's 0.09s. The standard already forbids that reading
   (14.2/3: after a name found to be a template-name, `<` is always
   the template-argument-list delimiter), and probing shows
   `recog-ref` implements the commit (`int x = T1 < 2;` is BAD for
   it); our parser was missing the rule — a semantic gap, not just a
   performance one.
3. Stringly hot-path recomputation: the mock name categories were
   re-derived by `string::find` on every probe of every identifier
   (many times per token under backtracking), and
   `override`/`final`/`""`/`0` were compared as strings at their use
   sites. These are definition-time facts of the terminal vocabulary.

Minor cleanups:

4. Dead code: `ParseForSuffix` rebuilt a `for-classic` node that the
   next statement overwrote; `ParseSelectionStatement` saved an
   `else_state` it never used (silenced with `(void)`).

Verified clean (no action needed):

- No fallback success paths, dummy outputs, or reference shelling: OK
  requires phases 1-7 plus a complete `translation-unit` parse to
  `ST_EOF`; all error paths converge on per-file BAD; the test-runner
  batch mode re-executes the real tool `main` with the same argv.
- No test-specific or source-shape gates: no fixture names, path
  probes, or token-count shortcuts anywhere in `dev/src/parse/`.
- Grammar coverage: every `pa6.gram` nonterminal reachable from
  `translation-unit` has a faithful parse implementation
  (cross-checked the full grammar against the per-area files); the
  documented ambiguity pins follow 6.8/8.2/14.2.3 and the handout's
  decl-specifier-seq type-name rule.
- Bracket-context stack soundness: every hard-bracket pop is guarded
  by a matching top check; openers and closers balance within the
  parse function that consumed the opener; angle pushes pair with
  close-angle-bracket consumption inside the same rule, so restore-by-
  truncation is correct.
- PA1-PA5 untouched except the predefined-macro move, which is pure
  code motion (`preproc` table now in `dev/src/predefined_macros.*`,
  byte-identical macro list); suites for pa1-pa5 unchanged and green.
- File audit: passes; the single [bad-division] warning on `parser.h`
  is the known heuristic false positive on a declarations-only header
  (one-line member declarations, no bodies).

## Changes Made

- `dev/src/parse/parse_token.h/.cpp`: added `EParseTokenFlags`;
  `BuildParseTokens` stamps mock categories, `ST_OVERRIDE`,
  `ST_FINAL`, `ST_EMPTYSTR`, `ST_ZERO` once per token; `ParseToken`
  carries the flags with a `HasFlag` query.
- `dev/src/parse/parser.h`, `parser_core.cpp`: added `MemoParse` with
  the `EMemoRule` slot enum and a `vector<bool>` failure bitmap keyed
  by (rule, position, innermost-bracket-is-angle); soundness argument
  documented at the definition.
- `dev/src/parse/parse_names.cpp`, `parse_expr.cpp`, `parse_decl.cpp`,
  `parse_declarator.cpp`: the 16 chokepoint rules
  (simple-template-id, id-expression, nested-name-specifier, type-id,
  declarator, abstract-declarator, parameters-and-qualifiers, the
  three specifier sequences, conditional/assignment/expression,
  cast/unary expressions, block-declaration) now run through
  `MemoParse` wrappers; mock-name and special-token gates query
  stamped flags.
- `dev/src/parse/parse_expr.cpp`: `ParseBinaryOperator` refuses the
  relational `<` when the previous token is a template-name
  identifier (14.2/3).
- `dev/src/parse/parse_stmt.cpp`: removed the dead `for-classic` node
  rebuild and the unused `else_state` save.
- `dev/src/parse/parse_class.cpp`: virt-specifier, class-virt-
  specifier, and pure-specifier checks use stamped flags.
- `cppgm.tests/course/pa6/template-name-lt-commits-bad.*` and
  `deep-template-arg-nest-bad.*`: new fixtures (refs generated with
  `recog-ref` via `scripts/run_all_tests.pl`) pinning the 14.2/3
  commit and the memoized linear handling of a 300-deep failing
  template-argument nest.
- `pa6/plan.md`: parsing-strategy, mock-lookup, and validation
  sections updated; added Architecture Review and Final Architecture
  Review.

## Validation

- `make test-report-through-pa6`: 285/285 (283 prior cases plus the
  two new pins), pa1-pa5 suites untouched and green.
- `perl scripts/cppgm_file_audit.pl --stage pa6 --paths dev/src`:
  passes (1 known heuristic warning on the declarations-only
  `parser.h`).
- Adversarial probes (all BAD, all matching `recog-ref`'s status):
  failing template-argument nests d=8: 9.85s -> 0.00s, d=1000:
  1.6s -> 0.01s, d=10000: 0.10s (reference: 0.09s at d=1000); 2000-
  deep sizeof, parameter-clause, and if-cascade nests all <0.05s.
- Success-path checks: 100k-deep parentheses OK in ~1.8s (512MB
  worker stack, unchanged); 16k-line realistic file 1.58s vs
  reference 0.34s (the project's usual ~5x factor, linear).
- 400-file randomized differential sweep vs `recog-ref`: 320/400
  identical; zero MINE-OK/REF-BAD (no over-acceptance); all 80
  MINE-BAD/REF-OK fall in the documented reference-lenience classes
  (no mock gating, junk recovery, post-PA6 constructs).
- 22 targeted probes (the handout's five 14.2.3 examples, the three
  plan.md ordering fixes, template-name `<` commit shapes, bitfield/
  virt-specifier/literal-operator forms) all match the expected status
  and the reference.
