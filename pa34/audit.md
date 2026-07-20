# PA34 Audit

## Audit Plan

Scope: the 27 PA34 commits after the PA33 audit (`9a8333b95..HEAD`),
~8,600 insertions across 83 files. All 272 pa34 tests and the
through-pa34 gate pass at audit start.

### Files to inspect

1. **Hosted environment + driver** — `dev/src/toolchain/hosted_env.{h,cpp}`,
   `dev/src/toolchain/compile_unit.{h,cpp}`, `dev/cppgm++.cpp` (driver
   `-E`/query-flag handling), `dev/gen_builtin_host_config.pl`.
2. **Hosted preprocessor** — `dev/src/preprocess.{h,cpp}`,
   `dev/src/preprocess_hosted.cpp`, `dev/src/macro_table.h`,
   `dev/src/pp_tokenizer.cpp`, `dev/src/numeric_literals.cpp`.
3. **Capability registry** — `dev/src/hosted_probes.{h,cpp}` (probe honesty:
   `__has_builtin` answers must match what sema accepts).
4. **Parser concessions** — `dev/src/ast/ast_parse_decl.cpp`,
   `ast_parse_declarator.cpp`, `ast_parse_expr.cpp`, `ast_parse_stmt.cpp`,
   `ast_parse_class.cpp`, `ast_parser_core.cpp`, `ast_text.cpp`.
5. **Traits and builtin templates** — `dev/src/sema/sem_trait.{h,cpp}`,
   `sem_builtin_template.cpp`, `type_builder.cpp` (SPEC_TRANSFORM),
   `sem_pack.cpp`, `template_deduce.cpp`, `template_order.cpp`.
6. **Builtin functions** — `dev/src/sema/sem_builtin.cpp`,
   `sem_hosted_builtin.cpp`, `const_eval_builtin.cpp`,
   `dev/src/lowering/lower_builtin.cpp`, `lower_unit.cpp`,
   `dev/src/x86/lowir_to_mir_atomic.cpp`.
7. **New language features** — `dev/src/sema/sem_stmt.cpp` (if constexpr,
   init-statements, structured bindings), `sem_lambda.cpp` (templated
   lambdas), `sem_aggregate.cpp` (designated initializers), `sem_ctor.cpp`,
   `sem_apply.cpp`, `sem_auto.cpp`.

### Performance risks

- Probe/trait evaluation triggering repeated class completion or
  instantiation without caching (`__is_nothrow_invocable` caching is
  claimed — verify).
- Hosted include search: per-include linear scans over the system dir
  chain are fine; verify no per-token or per-macro-expansion rescans of
  the hosted macro import.
- `ExpandTemplateArgumentPack` / `__integer_pack` expansion for large N.
- Aggregate realignment for designated initializers (per-element field
  scans → quadratic on wide aggregates).
- Atomic RMW compare-exchange loop expansion size.

### Ownership boundaries

- Hosted facts must enter only through the token stream / sema; nothing
  may bypass serialized LowIR into the backend (README requirement).
- `hosted_probes` is the single source of truth for `__has_*`; sema
  builtin lookup must not carry a second list that can drift.
- Semantic facts (linkage, declaring namespace, ABI names) must ride
  typed fields (`fn_owner`, `fn_c_linkage`), not pretty-printed strings.
- Earlier-stage fixtures (pa5 preproc, pa10 `--emit-*`) keep the unhosted
  environment; verify the gating is mode-based, not test-shaped.

### File-audit issues

- Stage-close splits (`sem_stmt.cpp`, `sem_aggregate.cpp`,
  `sem_builtin_template.cpp`, `sem_instantiation.h`, `template_order.cpp`)
  must be cohesive modules, not size-dodging fragments.
- Re-run `perl scripts/cppgm_file_audit.pl --stage pa34 --paths dev/src`.

### Cheat patterns to sweep

- Test-name / fixture-path / source-shape acceptance gates.
- Fallback success paths (catch-all "accept and continue" that would mask
  real errors), silent parse-and-discard beyond the documented asm/attr
  boundaries.
- Hardcoded trait/probe answers not derived from the class model.
- "First-wins tolerance" for duplicate definitions — verify it is scoped
  to the _FloatN alias-collapse case and not a general redefinition hole.
- `co_await`/`co_yield`/`co_return` contextual parse — verify instantiation
  fails with a diagnostic, not a crash, and gating is semantic.
- Timeout workarounds, retry loops, environment sniffing.

## Findings

### Fixed during audit

0a. **Exponential parse backtracking on parenthesized primaries**
   (`dev/src/ast/ast_parse_expr.cpp`). The paren primary attempted the
   PA34 fold-expression tail parse before the ordinary parenthesized
   read; both parse the same span, so each nesting level doubled the
   work (24 nested parens took ~50 s, 28 hung). This is exactly the
   avoidable-exponential class the audit targets.
0b. **`co_return` silently compiled as `return`**
   (`dev/src/ast/ast_parse_stmt.cpp` + `sem_auto.cpp`). The contextual
   `co_return expr;` concession rewrote to a plain `SK_RETURN` with no
   marker, so *instantiating* a template containing `co_return` built
   a function that returns the operand — a silent fallback-success
   path (co_await/co_yield already threw at analysis).
0c. **Non-template `if constexpr` skipped checking the discarded
   branch** (`dev/src/sema/sem_stmt.cpp`). C++17 [stmt.if] discards
   instantiation only inside templated entities; in plain code both
   branches must still be fully checked. An undeclared name in the
   discarded branch of a non-template function was silently accepted.
0d. **Position-blind fold short-circuit rescue**
   (`dev/src/sema/sem_trait.cpp`). `(f() || ... || true)` folded to
   constant `true` because *some* constant decided the result,
   silently dropping the `f()` call — wrong-code. The standard only
   skips operands after the deciding constant in evaluation order.
0e. **Stringly `std::is_nothrow_*_constructible<T>::value` fallback**
   (`dev/src/sema/sem_binder.cpp`). Any failing constant lookup whose
   *spelling* matched the libstdc++ idiom got the builtin trait's
   answer — the namespace was never checked, so a user's same-named
   trait in another namespace was silently answered too.
0f. **`__builtin_*_overflow` wrong-code corners**
   (`dev/src/sema/sem_hosted_builtin.cpp`,
   `dev/src/lowering/lower_builtin.cpp`). (i) A 128-bit result pointee
   was accepted but the i128 widening scheme can never report overflow
   for it (identity widening) — silent never-overflows. (ii) Operands
   were converted to the result type before the exact compute, so
   mixed-type calls (`add_overflow(-1, 0, &unsigned)`) diverged from
   GCC's infinite-precision semantics silently.
0g. **`__atomic_always_lock_free(16, 0)` answered true**
   (`dev/src/sema/sem_hosted_builtin.cpp`) with no 16-byte atomic
   codegen behind it; host g++ answers false without `-mcx16`.
0h. **Wrong (dead) f80 quiet-NaN image arm**
   (`dev/src/lowering/lower_builtin.cpp` LowerBuiltinFloatConstant):
   exponent 0 + no quiet bit produced a pseudo-denormal, not a NaN.
   Dead today (the PA29 float path intercepts `nanl`) but a latent
   wrong-code trap.
0i. **Non-constant atomic memory-order side effects dropped**
   (`dev/src/lowering/lower_builtin.cpp`): a runtime order expression
   was conservatively strengthened to seq_cst (correct direction) but
   never evaluated for its effects.

1. **Overly broad duplicate-explicit-specialization tolerance**
   (`dev/src/sema/sem_spec.cpp` BindClassExplicitSpecialization). The
   PA34 "_FloatN alias collapse" first-wins rule applied to *every*
   duplicate explicit specialization, so a plain
   `template<> struct S<int>{...}; template<> struct S<int>{...};`
   redefinition was silently accepted. The comment documented a narrow
   _FloatN scope the code did not enforce.
2. **Overly broad duplicate-inline-definition tolerance**
   (`dev/src/lowering/lower_unit.cpp` RegisterFunction). Same pattern:
   any second inline definition of one signature was dropped
   first-wins, not just the glibc per-format sets that collapse onto
   one floating type. `inline int f(float){...}` twice was silently
   accepted.
3. **Hosted angle includes searched the compile CWD**
   (`dev/src/preprocess.cpp` ResolveIncludeFile). The hosted mode
   skipped the including file's directory for `<...>` includes but
   kept the PA5 working-directory fallback, so a stray `endian.h` in
   the build CWD could shadow the system header. GCC never searches
   CWD for angle includes.

### Verified clean (no change needed)

- **Probe-registry honesty**: `hosted_probes.cpp` is a single registry
  consulted by both the preprocessor `__has_*` operators
  (`preprocess_hosted.cpp`) and sema builtin lookup
  (`sem_builtin.cpp`, `sem_hosted_builtin.cpp`, parser hooks,
  `lower_unit.cpp`). Every advertised builtin resolves in sema; the
  only drift is under-reporting (some accepted builtins answer
  `__has_builtin` 0), which is the honest direction.
- **Mode gating**: the pa5 `preproc` tool and `--emit-*` modes build a
  plain unhosted `Preprocessor`; hosted behavior enables only through
  `CompileOptions::hosted` / driver `-E`. No environment sniffing in
  `dev/src` (the only `getenv` is the documented `CPPGM_STDINC_PATHS`
  harness injection point in the tool entry).
- **`#include_next`**: each FileInstance records its producing
  search-chain slot; resume is O(1) at `slot + 1` with correct
  user/system offset math at the chain boundary.
- **`-D`/`-U`/`-include` order**: one command-ordered action vector,
  replayed after the host macro import; `-include` files process after
  all `-D`/`-U`, matching GCC.
- **Coroutine concession**: `co_await`/`co_yield`/`co_return` parse
  contextually only when a template-parameter scope is open
  (`AstParser::InTemplateScope`, a semantic gate) and only with an
  operand shape following; evaluation of `EK_COROUTINE_OP` throws a
  boundary error. Instantiating such a template exits nonzero with a
  diagnostic; plain functions keep the C++11 rejection.
- **`__is_nothrow_invocable` priority**: the builtin wins for
  unqualified/global uses; a namespace-scoped user declaration keeps
  priority. The gate is the declaring scope's kind (GCC-13's actual
  behavior), not a fixture check.
- **Builtin templates**: `__type_pack_element` /
  `__is_nothrow_invocable` template-ids cache per
  `TemplateArgumentKey` in `dependent_uses`; dependent uses defer like
  alias uses. GNU `_Complex` records key per element type in
  `complex_types_` (one synthesized record per TU, ordinary class
  layout rules, SysV-compatible pair layout).
- **`__integer_pack`**: O(N) expansion with per-element constant
  conversion; recognized by builtin call shape (its reserved name),
  dependent operands keep the pattern carry.
- **Leading-pack sealing** (`template_deduce.cpp`): the PA34 guard is
  semantic (`FixedPatternsMentionPack` + explicit-element presence);
  the through-pa34 gate exercises the earlier-stage deduction paths.
- **`template_order.cpp` split**: cohesive module (14.5.6.2/14.8.2.4
  partial ordering + redeclaration signature identity), not a
  size-dodge fragment; the spelling comparison it uses for
  non-composing dependent signatures models the standard's
  token-equivalence rule and predates PA34.
- **Probe error containment** (`sem_trait.cpp`): would-it-compile
  probes catch only within the probe, save/restore the
  unevaluated-operand flag, and compute answers from the class model
  (`ClassInfo` facts); no name-keyed canned answers.
- **Harness integrity**: no PA34 commit touched `scripts/`,
  `pa34/tests/`, `pa34/scripts/`, or any Makefile oracle; only
  `dev/Makefile` (host-config probe rule with a compare guard to keep
  mtimes stable) and `dev/frontend_source_sets.mk` (source lists).
- **File audit**: `perl scripts/cppgm_file_audit.pl --stage pa34
  --paths dev/src` passes (pre-existing style warnings only).

### Accepted boundaries (documented, honest failures)

- `__has_cpp_attribute` answers 0 (attributes are parse-and-discard;
  claiming support would be dishonest).
- `__has_include` with a macro operand throws a loud error instead of
  macro-expanding (no hosted header in the suite uses the idiom).
- The host-config probe (`gen_builtin_host_config.pl`) bakes an empty
  macro set if the host compiler probe fails — hosted tests then fail
  loudly; nothing fakes success.
- GNU float literal suffixes / `q`-suffix literals evaluate at
  long-double precision (binary128 evaluation is a documented
  boundary).

### Verified clean (agents' deep passes)

- **Preprocessor/hosted env** (full report above): registry coherence,
  mode gating, include_next math, -D/-U/-include ordering, no LowIR
  side channel, map-based macro storage. The `-dM` probe text parses
  once per process; `DefineHostMacroLine` re-tokenizes host macro
  bodies once per TU (constant cost, not asymptotic).
- **Builtin lowerings**: SWAR popcount/clz/ctz constants, bswap,
  f32/f64 inf/qNaN/sNaN images, fpclassify masks (f80 with explicit
  integer-bit handling) all bit-verified. Atomic memory orders are
  never weakened (loads are plain movs, stores XCHG at seq_cst, RMW
  lock-prefixed); compare-exchange writeback and the bitwise RMW
  CAS-loop re-read are correct. The 140 lines removed from
  `lowir_to_mir_flow.cpp` are byte-identical in
  `lowir_to_mir_atomic.cpp`; the prior dangling-pointer class is fixed
  via a stable `std::deque` home. `HostLibraryBuiltinSymbol` gates on
  `separate_compilation_`. Const-eval builtin folds fail loudly
  (`NotConstant`), never fabricate values. All 24 run tests map to
  real computation — no dummy lowerings.
- **Feature sema**: code moves verified byte-identical (sem_binder →
  sem_stmt 15/17 functions identical, the two changed only for
  init/constexpr forms; sem_lifetime → sem_aggregate identical plus
  the realign call). Structured bindings analyze the initializer once
  and error loudly on bitfields/bases/non-class; designated
  initializers reject out-of-order designators, run NSDMIs through
  the ctor path (NSDMI classes are not aggregates), realign in one
  linear pass. Init-statement desugar verified end-to-end (dtors on
  break, scoping over both arms). Templated-lambda deduction reuses
  the real machinery; uninvoked forms error loudly.

## Changes Made

0. Blocker fixes (finding 0a-0i):
   - `ast_parse_expr.cpp`: paren primary parses the ordinary
     expression first; the fold tail runs only on that failure and
     only when `FoldDotsAhead()` sees a depth-0 `...` before the
     matching paren (nested-paren parsing is linear again; 28 parens
     compile instantly).
   - `ast_parse_stmt.cpp` + `ast_expr.h` + `sem_auto.cpp`: contextual
     `co_return` marks `AstStmt::co_return`; `BindReturnStatement`
     throws the stage-boundary error on it (instantiation now exits
     nonzero; parse-only templates still accepted).
   - `sem_stmt.cpp`: `BindConstexprIfStatement` binds the discarded
     substatement into a detached holder when not instantiating
     (`instantiating_` == false), outside return-type deduction;
     template instantiations still skip it.
   - `sem_trait.cpp`: fold evaluation walks operands in the expanded
     expression's left-to-right evaluation order; a non-constant
     operand before the deciding constant throws the runtime-fold
     boundary error (constants deciding first still short-circuit).
   - `sem_binder.cpp`: `NothrowTraitShorthand` resolves the spelled
     trait through its (possibly qualified) prefix and answers as the
     builtin only for a class template declared inside namespace
     `std`; other traits keep their loud evaluation.
   - `sem_hosted_builtin.cpp` + `lower_builtin.cpp`: overflow builtins
     take each operand in its own promoted type; the lowering widens
     per-operand into i128 and checks representability against the
     result type (GCC infinite-precision semantics for every
     <= 64-bit combination — verified at runtime for mixed
     signed/unsigned add, u64*u64 mul, and mixed sub); 128-bit
     operands/results are a loud OutsideBoundary instead of a silent
     never-overflows.
   - `sem_hosted_builtin.cpp`: lock-free queries answer false for 16
     (host g++ default; no 16-byte codegen exists), extracted as
     `AnalyzeAtomicLockFreeQuery` (also keeps the dispatcher under
     the file-audit function-size limit).
   - `lower_builtin.cpp`: f80 NaN images pass the extra mantissa bit
     position (62 quiet, 61 signaling) so the arm is correct if ever
     reached; non-constant memory-order operands lower as effects
     (gated off the `__sync_*`/`__c11_atomic_init` forms whose last
     child is an operand, avoiding double evaluation); the
     builtin-name table scan exits early for names without the `__`
     prefix (hot-path cost removed for ordinary calls).
1. `dev/src/sema/decl_binder.{h,cpp}`: `ResolveBuiltinTypeName` is now
   an instance member that bumps `collapsed_alias_uses_` when a
   `_FloatN`/`__float128` spelling collapses to a standard floating
   type (the real distinct types `__int128`/`__builtin_va_list` do not
   count).
2. `dev/src/sema/sem_spec.cpp` + `dev/src/sema/template_info.h`:
   `BindClassExplicitSpecialization` snapshots the counter around its
   template-argument resolution; `ClassSpecialization::alias_collapsed`
   records collapse involvement on any landing. The first-wins path now
   requires collapse involvement; plain duplicates throw again.
3. `dev/src/sema/decl_function.cpp`, `sem_binder.cpp`, `sem_node.h`,
   `dev/src/lowering/lower_program.h`, `lower_unit.cpp`: function
   definitions record whether their signature composition used a
   collapsed alias (`SemNode::alias_collapsed` →
   `LowFunctionInfo::alias_collapsed`); the duplicate-inline-definition
   first-wins in `RegisterFunction` now requires the flag on either
   landing.
4. `dev/src/preprocess.cpp`: hosted angle includes skip the
   working-directory fallback (same `hosted_ && angled` gate the
   including-file-directory skip uses); unhosted PA5 order unchanged.

Reproducers checked: plain duplicate explicit specialization → error;
`S<double>`+`S<_Float64>` duplicate → first wins; plain duplicate
inline definition → error; `f(double)`+`f(_Float64)` inline duplicate
→ first wins; instantiated and plain-function `co_await` → nonzero
exit with diagnostic.

## Validation

- `perl scripts/cppgm_file_audit.pl --stage pa34 --paths dev/src`:
  **pass** (exit 0; 8 pre-existing style warnings, 0 fatal). The one
  fatal introduced mid-audit (a comment pushing
  `TryAnalyzeAtomicBuiltin` to 121 lines) was resolved by extracting
  the lock-free query as its own cohesive analyzer, not by shaving.
- `make test-report-through-pa34` on the final tree: **ALL TESTS
  PASSED (3189/3189)**, every stage pa1-pa34 green.
- pa34 suite standalone: 42/42 preproc, 206/206 compile, 24/24 run.
- Reproducers exercised end-to-end after the fixes:
  - 28-level nested parens compile instantly (was: >50 s / hang);
  - `co_return` instantiation → nonzero exit with boundary
    diagnostic; parse-only template accepted;
  - undeclared name in a non-template discarded `if constexpr` branch
    → error; template discarded branch still not instantiated;
  - `(g(vs) || ... || true)` → loud runtime-fold boundary error (was:
    silently folded to `true`); `(true || ... || g(vs))` still folds
    (legitimate short-circuit);
  - `my::is_nothrow_default_constructible<int>::value` → loud error;
    the `std::` spelling still answers as the builtin;
  - mixed-type overflow builtins verified at runtime
    (`add_overflow(-1, 0, &unsigned)` reports overflow,
    `mul_overflow(u64max, u64max, &u64)` reports overflow,
    `sub_overflow(5u, 9, &long)` yields -4 without overflow);
  - plain duplicate explicit specializations / inline definitions →
    error; `_FloatN`-collapsed duplicates → first wins.
