# PA20 Audit

## Audit Plan

Scope: the two PA20 commits (`9077425a2`, `9bb7abb4f`) on top of the
audited PA19 baseline (`30ab17a84`), reviewed against `pa20/plan.md`,
`pa20/README.md`, and the checked-in fixtures under `pa20/tests/`.

### Files to inspect

New sema files (full read):

- `dev/src/sema/const_eval.h` — value model (ConstObject /
  ConstPointer / EvalValue), engine interface.
- `dev/src/sema/const_eval.cpp` — object store, typed reads/writes,
  conversions, body registry.
- `dev/src/sema/const_eval_expr.cpp` — expression evaluation.
- `dev/src/sema/const_eval_stmt.cpp` — statements, calls, constructor
  evaluation, limits.

New lowering file (full read):

- `dev/src/lowering/lower_global.cpp` — split from `lower_unit.cpp`;
  global-definition rendering including the PA20 evaluated-image
  emission.

Changed files (diff review):

- `dev/src/sema/sem_binder.{h,cpp}`, `decl_binder.{h,cpp}` —
  `TryFullConstant` seam, constexpr variable evaluation, fallback
  sites (static_assert, array bounds, template value args).
- `dev/src/sema/sem_class.cpp`, `sem_member.cpp`,
  `sem_member_body.cpp`, `sem_special.cpp` — static-member images,
  in-class array bound completion, constexpr stamping on bodies.
- `dev/src/sema/template_args.cpp`, `template_deduce.cpp` — engine
  fallback for template value arguments with converted-constant
  semantics.
- `dev/src/sema/sem_expr.{h,cpp}`, `sem_cast.cpp`, `sem_call.cpp`,
  `sem_operator.cpp`, `sem_lifetime.cpp`, `const_expr.{h,cpp}`,
  `scope.h`, `class_info.h`, `sem_node.{h,cpp}` — noexcept operator,
  constexpr flags, supporting model changes.
- `dev/src/lowering/lower_unit.cpp`, `lower_function.{h,cpp}`,
  `lower_expr.cpp`, `lower_const.cpp`, `lower_program.h` —
  function-local statics (hoisted globals, guard shape), float
  rendering, multi-dim subscripts.
- `dev/src/ast/ast.{h,cpp}`, `ast_parse_decl.cpp` — init-declarator
  token spans.

### Cheating / fallback vectors to check

- Harness integrity: confirm `pa20/tests/`, `pa20/scripts/`,
  `pa20/Makefile` are untouched handout exports (git history).
- The engine must interpret analyzed SemNode trees at compile time to
  produce constant *values* — verify no path emits an interpreter,
  trampoline, or embedded payload into the LowIR output as a
  substitute for real lowering, and that runtime code for constexpr
  functions is still genuinely lowered when odr-used.
- Fallback success paths: `TryFullConstant` and the constexpr-variable
  path must fail hard where the standard requires (7.1.5p9), not
  silently accept; conversely quiet fallbacks must only exist where
  folding is an optimization (const-integral initializers).
- Test-specific gates: grep the new code for fixture names, token
  strings, or source-shape special cases (e.g. keying on a specific
  variable name or literal value from a fixture).
- The rejection fixture (non-constexpr conversion function as NTTP):
  verify rejection comes from the general constexpr-gating rule, not
  a special case.
- Local-static naming `__local_static__<fn>__<var>__tokens<A>_<B>`:
  harness-pinned; verify token indices come from real parser spans,
  not counted/patched to match specific fixtures.
- Timeout workarounds: check the step/depth limits are generous
  engine safety rails, not tuned down to skip slow fixtures.
- Commit message says "env-var debug hooks removed" — verify none
  remain in `dev/src` outside the test runner.

### Ownership boundaries to check

- Constant values: single owner. The engine owns ConstObjects and the
  constant-object store; `ScopeBinding.has_value` stays the integral
  fast path. Check the lowering reads stamped facts (images, folded
  scalars) off the sema model rather than re-deriving or re-parsing.
- Body registry identity: must match the lowering's identity scheme
  (canonical name + signature TypeEquals, fn_spec pointer identity);
  check it is not keyed on printed strings.
- Local-static routing: block-scope id uses route to hoisted globals
  "via per-scope keys" — check this is a typed (scope,name) mapping,
  not name-string matching that could collide across shadowed
  statics; check block-scope extern collapse is untouched.
- noexcept facts: new `fn_noexcept_decl` channel beside derived
  unwind facts — check the two channels don't drift or duplicate
  ownership of the same fact.
- Token spans: stamped once by the parser, threaded through the
  binder — check no other component re-derives spans.

### Performance risks to check

- Body registry incremental scan over `unit_.items/deferred/
  synthesized`: must be resumable (remember scan position), not a
  full rescan per call; lookup should be map-keyed, not linear per
  call site.
- Constant-object store lookups keyed by (scope,name): map access,
  no linear scans in expression evaluation hot paths.
- Engine evaluation of large arrays/objects: byte-image copies are
  fine at fixture scale; check for accidental per-element deep copies
  of whole objects inside loops (quadratic).
- `TryFullConstant` fallback: the PA11 AST evaluator remains the fast
  path; the engine must only run on fallback, not on every constant
  site.
- Image emission (`lower_global.cpp`): flattening should be one walk
  per global in layout order, not repeated scans of ptr_slots per
  byte.
- Local-static guard emission and (scope,name) global index lookups:
  no full-program walks per local-static use.

### File-audit issues to check

- `perl scripts/cppgm_file_audit.pl --stage pa20 --paths dev/src`
  must pass: all sources <=1500 lines, functions <=120 lines.
- The stage-completion commit split `lower_global.cpp` out of
  `lower_unit.cpp` and split `InstantiateFunctionBody` — verify these
  are real ownership splits, not mechanical fragment-hiding to duck
  the size checks (the split file must have a coherent boundary and
  the pieces must not share mutable state through back-channels).
- Check no implementation bodies moved into headers or other
  unchecked paths.

### Exit criteria

- fileAudit: `perl scripts/cppgm_file_audit.pl --stage pa20 --paths dev/src`
- tests: `make test-report-through-pa20`

## Findings

### Fixed

1. **Silent zero images when an evaluated image could not render**
   (`lower_unit.cpp` / `lower_global.cpp`): `BuildLifetimeHelpers`
   dropped the initialization actions of every `ImageBacked` global,
   but `RenderGlobal` attempted the image flattening *afterwards* and
   silently fell back to a `zero N` image when a slot could not render
   (engine-internal pointer targets). The two decisions disagreed:
   a static constexpr member whose image held pointers to string
   literals or into another constexpr object compiled successfully to
   an all-zero object with no initialization anywhere (verified:
   `Table::rows[2]` of `{int, const char*}` emitted `zero 32`, `main`
   read 0 instead of 2). Fixed on both sides: the render attempt now
   happens once, *before* the drop decision (`EnsureImageText`,
   cached per global; `TryImageBackedInit`), an unrenderable image
   keeps its dynamic initialization (for initializer-less 9.4.2p3
   storage definitions the analyzed in-class actions from
   `const_image_inits` run in `@__cppgm_init`), and the image
   renderer learned the two missing slot forms so the realistic cases
   render statically after all:
   - string-literal targets: `ConstObject` carries its originating
     `SN_LITERAL` (`literal_node`), and `AppendImageScalar` emits
     `ptr addr @__strlit__N` — byte-identical to what the reference
     emits for the same weak member image via the template path;
   - named-object targets: the engine attaches the symbolic
     (scope, name) identity beside the evaluated store object when the
     object also has runtime storage (`IdAddress`), so slots render
     `ptr addr @global`.

2. **Symbolic image slots dropped their byte offsets**
   (`lower_global.cpp`, `AppendImageScalar`): a pointer slot holding
   `&g[1]` rendered as `ptr addr @g` — the `+ 4` was silently
   discarded (verified: both elements of
   `constexpr int* const ps[2] = {&g[1], &g[2]}` emitted
   `ptr addr @g`). Fixed: slots render `ptr addr @sym + N`, the same
   offset form the checked-in refs pin since PA14
   (`addr @data + 800`) and PA17 (RTTI `ptr addr @... + 16`).

3. **Non-integral scalar static-member constants did not exist**
   (`sem_class.cpp`, `RecordStaticMemberObject`): only object-valued
   (class/array) in-class initializers evaluated into the constant
   store, so `static constexpr double x = 2.5;` was unreadable in
   constant expressions (`static_assert(S::x > 2.0)` rejected), and
   the 9.4.2p3 storage definition `constexpr double S::x;` hard-errored
   with "requires an initializer" — false rejections of core PA20
   boundary forms (floating and pointer constexpr evaluation). Fixed:
   scalar members evaluate through the same holder +
   `EvaluateVariableInit` path (gated on const/constexpr so mutable
   statics never enter the store), and the storage definition adopts
   the analyzed in-class initializer (`AdoptInClassInitializer`), so
   the definition renders the constant (`f64 ... = 2.5`,
   `ptr = addr @__strlit__N`) instead of erroring or zero-filling.

4. **Pointer-difference rejected same-object symbolic pointers**
   (`const_eval_expr.cpp`, `EvalPointerBinary`): with symbolic
   identities now attached (finding 1), the old `sym_scope` blanket
   rejection would have broken `&arr[2] - &arr[0]` over constexpr
   arrays; the relatedness test now matches the comparison rule
   (same object, same symbol).

5. **Dead/duplicated limit constant** (`const_eval.cpp`): an unused
   `kDepthLimit` duplicated the real one in `const_eval_stmt.cpp`;
   removed. Also removed the `OutsideBoundary` helper left unused in
   `sem_member_body.cpp` by the PA20 commits.

6. **File-audit sizes**: the fixes pushed `EvalLValue`,
   `BindQualifiedDeclarator`, and `BuildLifetimeHelpers` past the
   120-line function limit; each was split at a real seam
   (`IdAddress` mirroring the existing `MemberAddress` /
   `SubscriptAddress` helpers; `AdoptInClassInitializer` for the
   9.4.2p3 adoption rule; `TryImageBackedInit` + `ImageInitActions`
   for the image-vs-dynamic decision), not fragment-hidden.

### Verified non-issues

- **Harness integrity**: `pa20/tests/`, `pa20/scripts/`,
  `pa20/Makefile` contain only `Export assignments` commits.
- **No interpreter/VM/payload substitutes**: the engine interprets
  analyzed SemNodes at *compile time* to produce constant values;
  odr-used constexpr functions still lower to real LowIR bodies
  (weak, demand-emitted), and nothing embeds evaluation machinery in
  the output.
- **No fallback success paths**: constexpr initializers that fail to
  evaluate are hard errors (7.1.5p9, rethrown in
  `FinishConstexprObject`); quiet fallbacks exist only where folding
  is optional (const-integral initializers, non-constexpr const
  objects). `TryFullConstant` sites keep the PA11 AST evaluator as
  the primary path and only run the engine on its failure.
- **No test-specific gates**: no fixture names, token strings, or
  source-shape whitelists in the new code; the rejection fixture
  (non-constexpr conversion function as NTTP) fails through the
  general constexpr-body gate
  (`is_constexpr_fn || synthesized` in `EvalCall`) plus the 14.3.2
  constexpr check in `TryClassConversionConstant`.
- **Local-static naming**: `__local_static__<fn>__<var>__tokens<A>_<B>`
  derives from real parser token spans (`AstInitDeclarator.begin_token`
  stamped at parse position, end token from the declaration terminator),
  threaded parser -> binder -> SemNode; nothing is tuned per fixture.
- **Limits are safety rails, not timeout workarounds**: step limit
  1<<22, call depth 512 (fixture needs 128); no test-driven tuning.
- **No env-var hooks** outside the test runner; no shell-outs to
  reference binaries or host toolchains from `dev/src`.
- **Ownership**: constant values have one owner (the engine store +
  `ScopeBinding.has_value` integral fast path); the lowering reads
  stamped facts (`const_images`, `has_value`, `has_float`) and never
  re-derives or re-parses them. The `fn_noexcept_decl` channel keeps
  declared exception facts separate from the derived `fn_unwind_no`
  body facts (5.3.7 reads only the former). The body registry keys on
  specialization pointer identity, then (scope, name) + `TypeEquals`
  — the lowering's identity scheme, not printed strings.
- **Performance**: the body registry scans `items/deferred/synthesized`
  incrementally (positions remembered) and looks up via maps;
  the constant store is map-keyed; image flattening is one walk per
  global in layout order, attempted exactly once (`image_state`);
  `LowerUsedFunctions` keeps its rescan floor. Engine evaluation of
  non-constexpr const objects is attempted once per object and
  bounded by the step limit (folding fallback, failure swallowed).
- **Block-scope extern collapse**: local statics register under a
  per-scope key (`LocalStaticKey`, pointer-suffixed) that shadows the
  collapsed key only for hoisted statics; `HasGlobal` checks are
  exact-key lookups.

### Reference divergences (documented, intentional)

- The reference binary rejects out-of-class storage definitions of
  static constexpr members ("mismatched variable declaration") and
  address-offset image slots ("unsupported constexpr class member
  initializer"), and cannot evaluate float member constants through
  the template path. Where it agrees (string-pointer weak member
  images via a template definition, non-template float member
  static_asserts), the behavior is pinned by two new course tests;
  the remaining fixed forms follow the standard per the repo rule
  that refs are imperfect.
- `lowir2native` (a later-milestone backend) silently ignores
  `addr @sym + N` offsets — including the PA14-pinned
  `200-global-array-one-past-end-pointer` reference output, so the
  gap predates PA20 and affects the reference outputs identically.
  PA20's grading contract is canonicalized LowIR text, which pins the
  offset form.

## Changes Made

- `dev/src/sema/const_eval.h`: `ConstObject.literal_node` back-pointer;
  `IdAddress` declaration.
- `dev/src/sema/const_eval.cpp`: dead `kDepthLimit` removed.
- `dev/src/sema/const_eval_expr.cpp`: `StringObject` records its
  literal node; `IdAddress` split out of `EvalLValue` and attaches the
  symbolic (scope, name) identity to store-backed namespace/class
  objects; pointer difference uses the same-object/same-symbol
  relatedness rule.
- `dev/src/sema/sem_class.cpp`: `RecordStaticMemberObject` covers
  non-integral scalar members and gates on const/constexpr.
- `dev/src/sema/sem_lifetime.cpp`, `sem_binder.h`:
  `AdoptInClassInitializer` (folded-literal push + scalar in-class
  initializer adoption for storage definitions).
- `dev/src/sema/sem_member_body.cpp`: unused helper removed.
- `dev/src/lowering/lower_program.h`: `image_state`/`image_text`
  cache on `LowGlobalInfo`; `EnsureImageText`, `TryImageBackedInit`,
  `ImageInitActions` declarations.
- `dev/src/lowering/lower_global.cpp`: `AppendImageScalar` renders
  string-literal and symbolic slots with byte offsets;
  `EnsureImageText` one-shot render cache; `RenderGlobal` consumes it.
- `dev/src/lowering/lower_unit.cpp`: `BuildLifetimeHelpers` drops
  init actions only when the image actually renders
  (`TryImageBackedInit`) and falls back to the analyzed in-class
  actions for initializer-less storage definitions.
- `cppgm.tests/course/pa20/300-static-constexpr-member-image-string-pointers.*`,
  `310-static-constexpr-float-member-constant.*`: course tests pinning
  the fixed behaviors where the reference agrees (refs generated via
  `make -C pa20 ref-test TEST=course/pa20`).

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa20 --paths dev/src`:
  pass (2 pre-existing `bad-division` warnings in `parser.h` /
  `sem_binder.h`, both predating PA20).
- `make -C pa20 test`: 61/61 handout + 2/2 course.
- `make test-report-through-pa20`: 1551/1551 (baseline before audit
  fixes: 1549/1549; the two new course tests account for the delta).
- Probe programs (compiled with `dev/cppgm++`, images inspected and,
  where the backend supports the forms, executed via
  `reference-binaries/lowir2native`): static constexpr member arrays
  with string pointers, pointer arrays into runtime and constexpr
  globals with offsets, float and string-pointer scalar members with
  static_asserts and storage definitions — all emit correct constant
  images or fold correctly; the reference binary agrees byte-for-byte
  on the string-pointer weak-member image.
