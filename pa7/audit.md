# PA7 nsdecl — audit

## Audit Plan

Scope: commit 00631d4b7 (PA7 nsdecl) against `pa7/README.md`,
`pa7/pa7.gram`, and `pa7/plan.md`; shared-code changes that could touch
PA1-PA6 (`dev/frontend_source_sets.mk` is the only shared file in the
diff).

Files to inspect:

- `dev/nsdecl.cpp` — driver: real phases 1-7 per srcfile, no fallback
  success path, exact output framing (`<n> translation units`, per-TU
  start/end lines), EXIT_FAILURE on any error.
- `dev/src/sema/type.{h,cpp}` — factory rules (cv over arrays, cv
  dropped on references, reference collapsing, 8.3.5p5 adjustment,
  array-completion merge), `DescribeType` against the README's exact
  type-description grammar.
- `dev/src/sema/entity.{h,cpp}` — arena ownership, binding model,
  print-list ordering (variables, functions, namespaces, each by first
  declaration), the unnamed-member slot, implicit using-directives for
  unnamed/inline members, duplicated or dead state.
- `dev/src/sema/name_lookup.{h,cpp}` — 3.4.1 + 7.3.4 unqualified
  lookup (directive anchors, transitivity with the container fixed),
  3.4.3.2 qualified lookup (inline namespace set, directive recursion
  with a visited set), filter behavior.
- `dev/src/sema/decl_parser.{h,cpp}` — declaration dispatch lookahead,
  decl-specifier-seq counters vs the 7.1.6.2 table, declarator type
  composition order (prefix left-to-right, suffixes right-to-left,
  recursion into parenthesized sub-declarators), the 8.2p7 `(`
  disambiguation scan, array-bound evaluation (integral, non-UD,
  positive), redeclaration matching (qualified vs unqualified), the
  `(void)` rule, semantic actions for the 7.3 namespace forms.

Cheating checks:

- No test-specific or source-shape acceptance gates (grep for fixture
  names, srcfile-path probes, token-count or output-shape shortcuts).
- No fallback or dummy output: every TU description must come from a
  completed phase 1-7 pipeline plus a full semantic parse to EOF; all
  error paths exit EXIT_FAILURE; no embedded reference output and no
  shelling to `nsdecl-ref`.
- Grammar coverage: every `pa7.gram` production reachable from
  `translation-unit` has a real parse-plus-semantic-action
  implementation; no production silently dropped or over-accepted to
  dodge hard cases (cross-check the grammar file against the parser).
- The test-runner batch path must run the same `main` as the
  standalone tool (generic `-Dmain=test_runner_real_main` wrapper).

Regression checks:

- `dev/frontend_source_sets.mk`: only the nsdecl line changed; no
  other tool's object list touched.
- No shared `dev/src/*` file modified by the PA7 commit; pa1-pa6
  suites stay green in `make test-report-through-pa7`.

Performance risks:

- `UnqualifiedLookup` recomputes the transitive using-directive
  closure (with `set` allocations) on every lookup, including every
  `(` disambiguation scan — quadratic in (lookups × directives) for
  directive-heavy TUs; measure an adversarial input and fix if
  material (the closure only changes when a directive is added or a
  namespace is entered/exited, so it is cacheable by version).
- Deep declarator nesting `((((x))))` and long specifier/declarator
  chains — recursion depth and per-chunk allocation.
- `ScanIsTypeName` re-walks qualified names ahead of the cursor; check
  it cannot rescan quadratically on realistic shapes.
- `CollectInlineSet` allocates per qualified lookup — bounded by
  member counts; verify it is not on a hot loop.

Ownership boundaries:

- Semantic facts must live in the model, not in strings: type identity
  in `Type` nodes (never re-parsed from text), namespace identity as
  `Namespace*` (aliases bind the same object), entity print position
  recorded once at first declaration.
- One owner per fact: check `Namespace::is_unnamed` vs `name.empty()`
  duplication, and that nothing downstream re-derives variable-vs-
  function from anything but the binding/type.
- `SemaModel` arena owns all nodes; all other pointers non-owning;
  `TypePtr` sharing must not allow mutation after publication
  (immutable nodes, completion replaces the pointer).

File-audit issues:

- `perl scripts/cppgm_file_audit.pl --stage pa7 --paths dev/src` must
  pass; verify no exclusions or budget meta-files were added, file
  sizes are within limits without hidden fragments, and the
  pre-existing `parse/parser.h` [bad-division] warning (accepted in
  the PA6 audit as a heuristic false positive) is unchanged.

Differential validation:

- Re-verify against `nsdecl-ref` on synthesized inputs beyond the
  fixtures: transitive/anchored using-directives, inline-namespace
  qualified lookup, reopened-namespace ordering, array completion via
  qualified redeclaration, `(void)` through typedefs, reference
  collapsing through alias chains, literal array-bound forms, and the
  8.2p7 paren shapes (including the documented `int (::I)` divergence,
  which follows g++/8.2p7 and is outside fixture coverage).

## Findings

Blockers (fixed in this pass):

1. Stack-bound recursion on the main thread: declarator parsing (and
   `Declarator` destruction) recurses per parenthesization level, so
   nsdecl segfaulted near ~1M-deep `int ((((x))));` on the default
   8MB stack. PA6's audit already institutionalized the
   large-stack worker-thread strategy in recog for exactly this class;
   nsdecl now uses the same pattern (512MB virtual stack per srcfile).
   For scale: the reference segfaults at 20k deep; ours now does 1M in
   4.6s.
2. Hot-path recomputation: `UnqualifiedLookup` rebuilt the transitive
   using-directive closure — `set<pair>` and all — on every lookup,
   including every 8.2p7 `(` classification scan, making
   directive-heavy TUs quadratic in (lookups × directives): 2.98s on a
   3000-directive/3000-lookup adversarial input. The closure only
   changes when a directive is added (explicitly or implicitly via
   unnamed/inline member creation) or the scope chain changes, so it
   is now memoized per innermost scope (`DirectiveClosureCache`) and
   invalidated at the two mutation points: 1.13s on the same input
   (reference: 8.42s), 0.37s → 0.06s on a 1500-deep directive chain
   (reference: 2.23s).
3. Duplicated ownership: `Namespace::is_unnamed` restated
   `name.empty()` (and was never read after construction); the
   field and the parallel constructor parameters are removed — empty
   name is the single representation, `parent == null` already
   distinguishes the global namespace.
4. `pa7/plan.md` claimed the inline flag is "OR-ed on reopen"; the
   code (correctly — a disagreeing extension-namespace-definition is
   ill-formed per 7.3.1p2) fixes it at creation. The plan also called
   `int (::I)` "the one divergence" from the reference; the
   differential sweep showed the full divergence class is wider. Both
   corrected in the plan rather than the code.

Verified clean (no action needed):

- No fallback success paths, dummy output, or reference shelling:
  the only output path runs the real PA5 pipeline plus a complete
  semantic parse to EOF per srcfile; every error path (pipeline error,
  parse error, PTK_INVALID token) throws and exits EXIT_FAILURE; no
  fixture names, path probes, or output-shape shortcuts anywhere in
  `dev/src/sema/` or `dev/nsdecl.cpp`; the test-runner batch path
  re-enters the real `main` via the generic `-Dmain` wrapper.
- Grammar coverage: every `pa7.gram` production reachable from
  `translation-unit` is implemented with semantic actions (the seven
  declaration forms, both declarator hierarchies, the three
  parameter-declaration-clause shapes, cv/ptr-operator forms, literal
  bounds); the only intentional deviations accept ill-formed shapes
  the grammar alone cannot exclude, which is UB territory for PA7.
- Lookup semantics against the standard and `nsdecl-ref`: transitive
  directives with the container fixed (7.3.4p4), anchor at the nearest
  common enclosing namespace (7.3.4p2, including the standard's
  A/B/C/D example shape), qualified lookup over inline namespace sets
  with directive fallback (3.4.3.2), namespace-name lookups ignoring
  non-namespace bindings (3.4.6). 30+ synthesized probes ran identical
  to the reference; the disagreements split into reference bugs on
  well-formed input (8.2p7 `( type-name )` parameters, sibling
  directive shadowing — both follow the standard's own examples and
  g++ here) and ill-formed/UB inputs where neither behavior is
  required (qualified function declarators, pointer-to-reference,
  non-TT_LITERAL bounds). All are documented in `pa7/plan.md` under
  "Known reference divergences"; the agreeing neighborhood of each is
  pinned as course fixtures.
- Type model: factory rules verified against 8.3.1/8.3.2p6/8.3.4p1/
  8.3.5p4-5 (cv redistribution over arrays incl. multidimensional,
  cv dropped on references, && collapsing, parameter adjustment
  preserving element cv, `(void)` — including through typedefs — and
  named/variadic forms); composition order verified (prefix
  left-to-right, suffixes right-to-left, parenthesized recursion) on
  function-pointer/array shapes against the reference.
- Array bounds: integral non-UD literal with positive value enforced
  via literal type + value bytes (`LittleEndianValue`); signedness
  check covers the only reachable negative forms (char-typed
  literals); bool/char16/char32/wchar literals accepted per the
  converted-constant-expression rule.
- Regression surface: the PA7 commit touched no shared `dev/src/*`
  file; `dev/frontend_source_sets.mk` changed only the nsdecl line;
  pa1-pa6 suites green before and after the audit changes.
- File audit: passes; no exclusions, budget meta-files, or unchecked
  paths added; the single [bad-division] warning on the
  declarations-only `parse/parser.h` predates PA7 and was accepted in
  the PA6 audit as a heuristic false positive.

## Changes Made

- `dev/nsdecl.cpp`: each srcfile's pipeline + parse now runs on a
  512MB-stack worker thread (recog's audited pattern); errors
  propagate through the task struct and still exit EXIT_FAILURE.
- `dev/src/sema/name_lookup.h/.cpp`: `ActiveDirective` moved to the
  header; added `DirectiveClosureCache` (memo keyed by the innermost
  namespace, holder-invalidated); `UnqualifiedLookup` takes an
  optional cache and otherwise behaves identically.
- `dev/src/sema/decl_parser.h/.cpp`: the parser owns a mutable
  `DirectiveClosureCache`, passes it to both `UnqualifiedLookup` call
  sites in `ResolveComponents`, and invalidates it in
  `ParseUsingDirective` and on namespace creation.
- `dev/src/sema/entity.h/.cpp`: removed `Namespace::is_unnamed` and
  the `is_unnamed` parameters of `CreateNamespace` /
  `AddMemberNamespace` (empty name now means unnamed).
- `cppgm.tests/course/pa7/`: 14 new fixtures (refs generated with
  `nsdecl-ref` via `scripts/run_all_tests.pl`) pinning directive
  anchoring/transitivity (nested and sibling, plus a 200-deep chain),
  inline-namespace qualified lookup, alias-to-inline-member,
  using-declaration reuse, array completion (unqualified and
  qualified), `(void)`-through-typedef, reference collapsing through
  alias chains, the agreeing 8.2p7 paren shapes, literal bound forms,
  namespace-name lookup skipping entity bindings, and a 2000-deep
  parenthesized declarator.
- `pa7/plan.md`: corrected the inline-reopen claim, replaced the
  single-divergence note with the "Known reference divergences"
  section, updated validation for the new course tests, added
  Architecture Review and Final Architecture Review.

## Validation

- `make test-report-through-pa7`: 324/324 (310 prior cases plus the
  14 new pins); pa1-pa6 suites untouched and green.
- `perl scripts/cppgm_file_audit.pl --stage pa7 --paths dev/src`:
  passes (1 pre-existing heuristic warning on the declarations-only
  `parse/parser.h`, unchanged from PA6).
- Differential sweep vs `nsdecl-ref` (30+ synthesized inputs):
  identical output on every well-formed probe except the two
  standard-backed divergence classes documented in plan.md, both
  re-verified against g++ (`int(I)` parameter: g++ rejects `f(42)`
  and accepts a function-pointer argument) and the standard's
  examples ([dcl.ambig.res] `void f(int(C))`, [namespace.udir]p2
  A/B/C/D).
- Adversarial probes after the fixes: 1500-deep directive chain
  0.06s (was 0.37s; reference 2.23s); 3000 directives × 3000 lookups
  1.13s (was 2.98s; reference 8.42s); 200k-deep parens 0.98s and
  1M-deep 4.59s (previously segfault above ~200k; reference
  segfaults at 20k); all outputs identical to the reference where it
  survives.
