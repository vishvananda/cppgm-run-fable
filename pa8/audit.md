# PA8 nsinit — audit

## Audit Plan

Scope: commit 7013155a9 (PA8 nsinit) against `pa8/README.md`,
`pa8/pa8.gram`, and `pa8/plan.md`; shared-code changes that could touch
PA1-PA7 (`dev/frontend_source_sets.mk` and `dev/nsdecl.cpp` are the
only shared files in the diff; nsdecl now compiles the PA8 sema set).

Files to inspect:

- `dev/nsinit.cpp` — driver: real phases 1-7 per srcfile on the
  large-stack worker, one shared `Program`, no fallback success path,
  binary image written only after every TU parses, EXIT_FAILURE on any
  error.
- `dev/src/sema/program.{h,cpp}` — entity arena and linking key map
  (path + name + signature, external only), ODR records (variables
  once per program, inline functions once per TU), block lists in
  creation order, layout (alignment, never-defined-variable skipping,
  function stubs 4/4), relocation (unplaced targets resolve to 0),
  image write.
- `dev/src/sema/expr.{h,cpp}` — literal decoding, value-category and
  constant-value annotation, the 5.19 lvalue-to-rvalue gate (const
  integral / constexpr, visible initialization, volatile rejection),
  the clause-4 conversion driver (arithmetic 4.5-4.9, qualification
  4.4, pointer 4.10, boolean 4.12), overload resolution by exact type
  (13.4), constant encoding to object representation.
- `dev/src/sema/init.{h,cpp}` — scalar copy-init (8.5p17), character
  arrays from string literals (8.5.2: element match, completion,
  length, zero fill), reference binding (8.5.3p5 direct-bind rules,
  temporary materialization, const-ref/rvalue-ref gating), default
  initialization rejections, static_assert and array-bound evaluation.
- `dev/src/sema/decl_parser.{h,cpp}` — pa8.gram coverage from
  translation-unit (expressions, initializers, static_assert,
  function-definition); specifier accumulation vs 7.1.1/7.1.5/7.1.2
  (combination and duplication rules); declaration actions
  (redeclaration kind/type/linkage conflicts, overload sets,
  using-declaration/alias/namespace conflicts); qualified
  declarator-id enclosure check and the 3.4.3p3 scope switch; linkage
  computation (3.5p3-4).
- `dev/src/sema/type.{h,cpp}` — strict factories, structural equality,
  TypeSize/TypeAlignment vs the handout ABI table (with overflow
  checks), qualification-conversion rule, redeclaration merge.
- `dev/src/sema/name_lookup.{h,cpp}` — 3.4.1p2 ambiguity and overload
  merging across paths, MemberLookup for qualified redeclaration.

Cheating checks:

- No test-specific or source-shape acceptance gates (grep for fixture
  names, srcfile-path probes, getenv gates outside the test runner,
  output-shape shortcuts).
- No dummy/templated image output: the image must be laid out from the
  typed slot list built by a full parse of every TU; no embedded
  reference bytes, no shelling to `nsinit-ref` or a host compiler;
  every error path exits EXIT_FAILURE.
- Grammar coverage: every pa8.gram production reachable from
  translation-unit has a real parse-plus-semantic-action
  implementation; diagnosable ill-formed programs inside the grammar
  must fail (look for accept-by-omission: unchecked specifier
  duplication, typedef-bodied function definitions, dropped
  thread_local facts).
- The test-runner batch path must run the same `main` as the
  standalone tool (generic `-Dmain` wrapper, binary outfile compare).

Regression checks:

- `dev/frontend_source_sets.mk`: only the nsdecl/nsinit lines changed.
- nsdecl shares the extended DeclParser: PA7 suites must stay green
  (`make test-report-through-pa8` covers pa1-pa7), and the added
  diagnostics must fire only on PA7-UB inputs.

Performance risks:

- `ParseSimpleDeclaration` copies the whole lexical scope chain per
  init-declarator to support the qualified-declarator scope switch —
  O(namespace depth) per declarator, quadratic-ish on deep-nesting
  adversarial inputs; measure and restructure if material.
- Linking-key construction renders DescribeType per first declaration
  — bounded by declaration count; verify no per-lookup rendering.
- `WriteImage` builds the whole image in memory — inherent to the
  format (bytes must be written); overflow-checked; verify no
  quadratic placement scan.
- Directive-closure memoization from PA7 must still be wired through
  the new lookup call sites.

Ownership boundaries:

- Initial-value facts live once, typed, on ImageSlot (kind, bytes,
  target+addend, constness, defining TU); the evaluator and the image
  writer must both read exactly these — no re-deriving constness or
  values downstream, no string-encoded semantic facts (the linking key
  is an index derived from typed facts, never parsed back).
- One owner per fact: specifier flags must land on the entity at
  declaration time (constexpr/inline/linkage) and redeclarations must
  merge into the one shared entity through every path (unqualified,
  qualified, cross-TU link) — check for paths that drop flags.
- Program owns entities/temporaries/strings; SemaModel owns the
  per-TU namespace tree; all other pointers non-owning; TypePtr nodes
  immutable after publication (completion replaces the pointer).

File-audit issues:

- `perl scripts/cppgm_file_audit.pl --stage pa8 --paths dev/src` must
  pass; no exclusions or budget meta-files added; file sizes within
  limits without hidden fragments; the pre-existing accepted
  [bad-division] heuristic warning on `parse/parser.h` unchanged.

Differential validation:

- Probe `nsinit-ref` (and g++ -std=c++11 where the reference is known
  lenient) on the suspected diagnosis gaps: duplicated specifiers
  (`static static`, `extern extern`, `thread_local thread_local`,
  `const const`), thread_local agreement across declarations,
  function definition through a function typedef, constexpr added on
  a qualified or cross-TU redeclaration feeding 5.19 reads.
- Re-run the plan's divergence triage spot checks after any fix to
  confirm no new divergence class appears.

## Findings

Blockers (fixed in this pass):

1. Performance: `LinkNewEntity` rendered the home namespace path with
   recursive string concatenation (`NamespacePath`) for every first
   declaration - O(depth^2) bytes per declaration - plus an O(depth)
   `InsideUnnamedNamespace` walk and an O(depth) copy of the lexical
   scope chain per init-declarator. A 549KB input (20k-deep namespaces,
   20k members) ran for over five minutes before being killed.
   Namespace identity is now interned once per namespace creation
   (`Program::InternNamespace`, `Namespace::link_id`; negative id =
   unnamed context), and the qualified-declarator scope switch swaps
   the chain instead of copying. Same input: 0.63s; doubled input:
   0.77s (linear). The reference segfaults on the 20k input and is
   superlinear below that (600-deep: 1.5s vs our 0.05s).
2. Missed diagnoses (diagnosable ill-formed programs inside pa8.gram
   exited EXIT_SUCCESS):
   - duplicated decl-specifiers (`static static`, `extern extern`,
     `thread_local thread_local`, dup `typedef`/`constexpr`/`inline`,
     `const const`, `volatile volatile`, and `int* const const`) -
     7.1.1p1, 7.1.6.1p1, 7.1.6.2p2; g++ -std=c++11 rejects all, the
     reference accepts all (standard wins on non-fixture inputs).
   - function definition through a function typedef (`typedef void
     F(); F f {}` and `F (f) {}`) - 8.3.5p10; both g++ and the
     reference reject. Fixed by requiring the definition's declarator
     to spell a type chunk (`HasDeclaratorChunks`); the legal
     `void (g()) {}` and typedef-declared `F f; void f() {}` forms
     stay accepted.
   - thread_local disagreement between declarations of one variable -
     the entity did not record thread storage duration at all (a
     dropped semantic fact). g++ and the reference reject the same-TU
     mismatch; we also reject it cross-TU (documented divergence: the
     reference is lenient there; real linkers are not).
3. Dropped fact on redeclaration paths: `DeclareQualified` and the
   cross-TU merge in `LinkNewEntity` did not propagate `constexpr`
   onto the shared entity (the unqualified same-TU path did), so a
   well-formed program reading a constexpr pointer declared via a
   qualified or cross-TU redeclaration was wrongly rejected by 5.19
   (`namespace A { extern const char* const p; } constexpr const
   char* A::p = "abc"; static_assert(A::p, "x");` exited FAILURE;
   g++ and the reference accept, and our fixed image matches the
   reference byte-for-byte).
4. Interface honesty: `Program::WriteImage` was declared const while
   mutating slot offsets through stored non-const pointers during
   layout; the const is removed.

Verified clean (no action needed):

- No fallback success paths, dummy output, or reference shelling: the
  only output path lays out and writes the image from the typed slot
  lists built by a complete parse of every TU; every error path
  throws and `main` maps any throw to EXIT_FAILURE; no fixture names,
  path probes, or environment gates in `dev/src/sema/` or
  `dev/nsinit.cpp`; the batch test path re-enters the real `main` via
  the generic `-Dmain` wrapper and compares the binary outfile.
- Grammar coverage: every pa8.gram production reachable from
  translation-unit is implemented with semantic actions (expressions
  incl. parenthesized and qualified id-expressions, initializers,
  static_assert with literal message, function definitions, the
  parameter-clause forms, array bounds as converted constant
  expressions, qualified declarator-ids with the 3.4.3p3 scope
  switch).
- Image format against the handout: "PA8\0" magic; Block 1 in
  first-declaration order skipping never-defined variables but
  keeping all declared functions (stubs "fun\0", 4/4 mock
  size/alignment); Block 2 temporaries in creation order; Block 3
  one object per string-literal token in token order (static_assert
  messages excluded); ABI sizes/alignments per the table with
  overflow-checked array sizes; pointers/references as 64-bit LE
  offsets; relocations of unplaced targets resolve to 0.
- 5.19/clause-4 semantics: constant reads gate on const-integral or
  constexpr, non-volatile, visible (same-TU) constant initialization;
  references deref through their recorded constant binding;
  non-constant initializers of well-formed programs zero-initialize
  (handout) while constant-requiring contexts reject them; the
  conversion driver covers integral/floating/boolean/pointer/
  qualification conversions with null-pointer-constant handling.
- ODR and linkage: variables define once per program, functions once
  except inline (once per TU); internal entities (static, unnamed
  namespace, const non-extern) are per-TU and unkeyed (the
  130-staticvar shape); static-after-external and constexpr/return-
  type disagreements diagnosed; overload sets merge across lookup
  paths with 3.4.1p2 ambiguity diagnosis.
- Regression surface: nsdecl shares the extended parser but builds a
  throwaway per-TU Program; pa1-pa7 suites green before and after the
  audit changes (373/373 through-report).
- File audit: passes; no exclusions, budget meta-files, or unchecked
  paths; the single pre-existing accepted [bad-division] warning on
  `parse/parser.h` is unchanged.

## Changes Made

- `dev/src/sema/program.{h,cpp}`: added `InternNamespace` (program-
  wide namespace identity map); `WriteImage` no longer const.
- `dev/src/sema/entity.{h,cpp}`: `Namespace::link_id` (interned id,
  negative = unnamed context) replaces the removed `NamespacePath`
  and `InsideUnnamedNamespace` helpers; `DeclaredEntity::
  is_thread_local` records thread storage duration.
- `dev/src/sema/decl_parser.{h,cpp}`:
  - duplicate-specifier diagnosis in `ConsumeSpecifierKeyword` (one
    flag per keyword, throw on repeat) and in the ptr-operator
    cv-qualifier-seq;
  - `HasDeclaratorChunks` + 8.3.5p10 check in
    `ParseFunctionDefinition`;
  - thread_local agreement in `CheckRedeclarationSpecifiers`;
    `LinkNewEntity` stores the flag at creation;
  - constexpr propagation on the qualified (`DeclareQualified`) and
    cross-TU (`LinkNewEntity` merge) redeclaration paths;
  - linking keys built from `link_id` instead of the rendered path;
  - `EnterQualifiedDeclaratorScope`/`RestoreDeclaratorScope` swap the
    scope chain through a member save slot instead of copying it per
    init-declarator.
- `cppgm.tests/course/pa8/`: 8 new fixtures (refs generated with
  `nsinit-ref` via `pa8/scripts/run_all_tests.pl`): typedef-fn-def
  (reject), typedef-fn-decl (accept, incl. `void (g()) {}`),
  threadlocal-mismatch (reject), threadlocal-agree (accept),
  constexpr-qualified and constexpr-crosstu (accept, image-compared),
  cv-through-typedef (accept), deep-namespaces (600-deep performance
  shape).
- `pa8/plan.md`: data-model bullet updated for interned ids; new
  divergence entries (duplicate specifiers, cross-TU thread_local);
  validation updated for the course fixtures; Architecture Review and
  Final Architecture Review added.

## Validation

- `make test-report-through-pa8`: 373/373 (365 prior cases plus the 8
  new course pins); pa1-pa7 untouched and green.
- `perl scripts/cppgm_file_audit.pl --stage pa8 --paths dev/src`:
  passes (1 pre-existing accepted heuristic warning on
  `parse/parser.h`).
- Differential probes vs `nsinit-ref` and g++ -std=c++11: all eight
  duplicate-specifier shapes now reject (g++ agrees; reference is
  lenient - documented); typedef-bodied definitions and same-TU
  thread_local mismatches reject (reference agrees); the constexpr
  propagation cases accept with byte-identical images; legal
  neighbors (`constexpr const int`, `int* const volatile`,
  `static thread_local` / `thread_local extern`, redundant cv through
  a typedef, multi-TU overload definitions) all accept with
  byte-identical images.
- Adversarial performance: 20k-deep/20k-member input 0.63s (was >5
  minutes, killed; the reference segfaults); 40k/40k 0.77s (linear
  scaling); 600-deep course pin 0.05s vs the reference's 1.5s.
