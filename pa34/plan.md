# PA34 Plan: Hosted Source/Header Compatibility

## Goal

Make `cppgm++` preprocess (`-E`) and compile (`-c`) hosted sources: import
the host compiler's predefined macro/include environment, add the hosted
preprocessor operator set, accept the GNU/Clang parser concessions the
selected hosted headers use, and implement the builtin traits/transforms/
intrinsics those headers and the bootstrap-facing sources exercise. The
object path stays the PA32/PA33 host-ABI pipeline; PA34 adds no new object
format.

Baseline at plan time: all 42 preproc tests fail (`-E` unimplemented), 149
of 206 compile tests fail, all 24 run tests fail; everything through pa33
passes.

## Ownership boundaries

1. **Host environment config** (`dev/gen_builtin_host_config.pl` →
   `obj/generated/cppgm_builtin_host_config.h`, consumed by a new
   `dev/src/toolchain/hosted_env.{h,cpp}`): the starter-kit probe captures
   the host compiler's `-dM -E` predefined macros and `-v` standard include
   paths at build time. `dev/Makefile` gains the same generation rule the
   pa34 Makefile already has, so the tested `dev/cppgm++` binary embeds the
   probe. `hosted_env` parses the `-dM` text into (name, definition-line)
   pairs — skipping the names our own predefined set owns (`__cplusplus`,
   `__DATE__`, `__TIME__`, `__FILE__`, `__LINE__`, `__STDC_HOSTED__`) — and
   exposes the system include dir list: `CPPGM_STDINC_PATHS` env entries
   (colon-separated, the test harness's injection point) first, then the
   baked host paths. Only `cppgm++` links this module; the pa5 `preproc`
   tool keeps the unhosted PA5 environment.
2. **Preprocessor hosted mode** (`dev/src/preprocess.{h,cpp}` + a new
   `dev/src/preprocess_hosted.cpp` for the hosted-only handlers): a
   `HostedOptions` switch on `Preprocessor` enables
   - `__has_include` / `__has_include_next` / `__has_builtin` /
     `__has_feature` / `__has_extension` / `__has_attribute` /
     `__has_cpp_attribute` / `__building_module` folded in controlling
     expressions before `defined` folding and macro expansion (operand
     never expanded, mirroring `defined`); the operator names register as
     builtin macros so `#ifdef __has_builtin` and `defined(__has_builtin)`
     are true, while stray text occurrences stay inert identifiers;
   - `#include_next` and system include search: quoted/angle search stays
     the PA5/PA29 order (file dir → `-I` dirs → CWD) with `-isystem` dirs
     and the hosted system dirs appended; each FileInstance records which
     search-chain dir produced it so `#include_next` resumes strictly after
     that dir;
   - `#warning` (gcc-style note to stderr, processing continues);
   - `-include <file>` pre-includes processed inside the main file's
     token stream before its first token.
   Unknown pragmas and unknown `_Pragma` operators are already ignored by
   the PA5 layer; conditional-group semantics are untouched.
3. **Capability registry** (`dev/src/hosted_probes.{h,cpp}`, shared by the
   preprocessor and sema): single source of truth answering "does this
   compiler implement builtin X / trait X / feature X / attribute X". The
   `__has_*` operators and the sema builtin-identifier lookups both read
   it, so probe answers can never drift from what sema actually accepts.
   The answers are honest: `__has_builtin(__builtin_trap)` is 0 until the
   builtin exists.
4. **Driver** (`dev/cppgm++.cpp`): `-E` emits the PA5 `preproc N` / `sof` /
   posttoken / `eof` stream (to `-o` or stdout); query flags (`--version`,
   `-v`, `-dumpmachine`, `-dumpversion`, `-print-search-dirs`) answer
   directly; `-D`/`-U`/`-include`/`-isystem` are collected in command order
   and applied to the hosted preprocessor for `-E`, `-c`, and link modes;
   `-pthread` becomes benign. `--emit-*` modes keep the unhosted PA10
   environment (their fixtures pin it).
5. **Compile mode** (`dev/src/toolchain/compile_unit.{h,cpp}`):
   `CompileOptions` grows the hosted preprocessor configuration (ordered
   define/undef actions, pre-include files, isystem dirs, hosted predefined
   macro import). The pipeline stays source → tokens → AST → sema → LowIR →
   MIR → object; hosted facts only change the token stream in, never add a
   side channel around serialized LowIR. The built-in runtime library TU
   keeps the unhosted environment.
6. **Parser concessions** (`dev/src/ast/ast_parse_*`): GNU `__attribute__`
   and C++11 `[[...]]` in the hosted positions (local/for-init/range/
   condition declarations, post-declarator parameters, namespace prefix/
   suffix, structured members), `__decltype`, `__typeof__`, `__float128` /
   `_Float128` / `__int128` (with postfix sign specifier), Clang
   nullability/block-pointer declarators, `_Atomic`, `_Complex`,
   deduction-guide declarations, fold-expressions, structured bindings,
   templated lambdas, designated initializers. Each concession lands as
   grammar + AST shape with real semantic binding, not source-text special
   cases.
7. **Builtin traits/transforms/intrinsics** (`dev/src/sema/`): trait
   identifiers (`__is_*`, `__has_*` sema forms) as constant boolean
   expressions over the class-info/type model; transforms
   (`__underlying_type`, `__remove_*`, `__decay`, `__type_pack_element`,
   `__make_integer_seq`/`__integer_pack`) as builtin type constructs
   evaluated during substitution; builtin functions through the existing
   `ResolveBuiltinFunction` lazy-declaration path plus lowering support
   where evaluation is inline (bswap, popcount/clz families, expect,
   assume_aligned, offsetof, atomic/sync families).

## Validation plan

- Fast loop: `cd pa34 && make check TEST=tests/preproc/<case>.t` (and
  `tests/compile/...`), then `make test-report ACTIVE_TEST_REPORT_PAS=pa34`.
- Gate: root `make test-report-through-pa34` after each cluster of shared
  preprocessor/parser/sema changes; earlier-stage regressions block.
- Preproc refs are host-agnostic by harness check; never pin host values.
- File audit: `perl scripts/cppgm_file_audit.pl --stage pa34 --paths
  dev/src` on every new/split file.

## Staging

1. [done] Hosted env + driver `-E`/query + hosted preprocessor operators →
   preproc bucket green (42/42).
2. [done] Hosted compile plumbing (`-c` sees host macros/system dirs/flags)
   → host-macro compile tests green.
3. [in progress] Parser concessions and builtin traits/transforms in
   clusters, running the through gate between clusters.
4. Run-suite builtins (link+execute against host libstdc++) last; they
   reuse the PA33 host-ABI object path.

## Progress and next steps (updated after the trait-core landing)

Done beyond staging 1-2: builtin transform family (SPEC_TRANSFORM
dispatch in type_builder.cpp), EK_BUILTIN_TRAIT structural traits
(sema/sem_trait.cpp shared by SemExprAnalyzer and the PA11 const-expr
walker), class-head `final`, trait-vs-functional-cast template-argument
disambiguation, GNU alias keywords (normalized in the AstParser token
copy), namespace prefix/suffix attributes, statement/specifier-position
[[...]] attributes, GNU asm statements and asm labels (parse-and-discard
for compile acceptance: inline-assembly codegen and asm-label symbol
renaming are documented boundaries for PA36 to revisit). ~115 compile +
24 run failures remain.

Cluster 4 (builtin function families) landed: fixed-signature builtins
are table-driven in sem_builtin.cpp (bswap/clz/ctz/popcount families,
expect, assume_aligned, prefetch, string/memory functions, float
constants, fences); C-library-backed builtins carry host libc symbol
names under separate compilation (lower_unit.cpp
HostLibraryBuiltinSymbol); magic-typed builtins (addressof, clzg/ctzg/
popcountg, overflow family, operator_new/delete, fpclassify, invoke,
offsetof) analyze in sem_hosted_builtin.cpp; constant folds live in
const_eval_builtin.cpp (hooked from EvalCall, with a PA20 fallback in
the scalar variable-template initializer path); inline lowerings live
in lower_builtin.cpp (SWAR popcount, smear clz, bswap op, i128
overflow check, per-format fpclassify rows, storage-image float
constants, ABI-named allocation calls, LowIR fences).
`__alignof__`/`__alignof` normalize to alignof. ~82 failures remain.

Hosted-header cluster landed: the run suite's hosted headers
(cstddef/cstdlib/cstring/cstdio/cassert/cmath/csignal) compile and
run. That required: probe operators folding to a fixpoint through
macro expansion (glibc's __glibc_has_attribute forwarding idiom) and
surviving `defined __has_feature`; hosted angle includes skipping the
including file's directory (bits/ headers include <unistd.h>);
GNU floating-literal suffixes (f16/bf16/f32/f64/f128/w/q families);
_FloatN/__float128 type names mapped to the nearest evaluated types
with first-wins tolerance for the alias-collapsed duplicate explicit
specializations and duplicate identical inline definitions;
__extension__ in declaration/expression positions; __typeof__ (a
reference-stripping SPEC_DECLTYPE variant with expression or type
operands); extern "C++" restoring C++ linkage inside extern "C";
namespace-scope using-imports merging with own overloads (per-slot
fn_owner keeps each overload's declaring namespace for host ABI
names, and per-slot fn_c_linkage decides item linkage); glibc math
builtins (full three-variant family) through the hosted_probes
LibcBuiltinSignature registry lowering to host libm calls; float
classification/comparison queries (isinf/isfinite/isnormal/signbit/
isgreater...) as inline f80 bit tests and unordered-aware compares;
__builtin_FILE/LINE/COLUMN as hosted spelling-site macros;
__builtin_FUNCTION/__func__/__PRETTY_FUNCTION__ as synthesized
name literals; GNU __null. Fixed a latent void-conditional lowering
bug (the then arm fell through into the else arm). 53 failures
remain.

Atomics landed: _Atomic(T) parses as a type operand (atomicity is
dropped; the operators carry the semantics), the __c11_atomic_*,
__atomic_*, and __sync_lock_* families analyze as magic-typed
builtins over the pointee type and lower onto the LowIR atomic
instructions; the bitwise read-modify-write forms expand as
slot-based compare-exchange loops (MIR temps are block-local); the
lock-free queries fold from their constant sizes. Registered builtin
names never read as type-names in declaration disambiguation. Fixed
a latent dangling-instruction bug in the MIR atomic-load rewrite
(the deferred single-use load kept a pointer to a stack-local copy).
43 failures remain: templated lambdas (6), structured bindings (4),
designated initializers (3), deduction guides (3), trait/invocable
corners (~8), integer_pack/type_pack_element corners (~6),
__complex__ (2), and misc parser/sema items.

Also landed: deduction-guide declarations (template-head/explicit/
plain forms parse and are accepted; guides only affect class template
argument deduction, a later hosted stage), __is_identifier answering
0 for language keywords, and [[...]] attributes between an alias
name and `=` (trait corners landed:
implicit-default triviality, defaulted-assign triviality, the
implicit ctor's computed noexcept specification, and the
is_nothrow_*_constructible shorthand).

The final clusters all landed; `make test-report-through-pa34` is
green (see the closing summary at the end of this file). The
historical cluster notes below record what each needed:

1. **Templated lambdas** (6): `[]<class U>(...)` — parse the template
   head into AstLambda; the three instantiated tests are all
   immediately-invoked, so deduce the lambda's template arguments at
   the call site and bind the closure with them in scope (the
   parse-only tests never instantiate their enclosing templates).
2. **Structured bindings** (4): `auto [a, b] = expr` over class
   members with real reference semantics.
3. **Designated initializers** (3): `.name = value` braced elements
   (reorder against the aggregate's fields with value-initialized
   holes at the braced-to-class conversion points) plus C compound
   literals `(T){...}`.
4. **C++17 statement forms** (2-3): if constexpr (discarded branches
   must not instantiate), if/if-constexpr init-statements including
   the alias form `if constexpr (using A = ...; A::value)`,
   conditional explicit(bool) on constructors (store the expr on
   AstMemberSpecifier; evaluate per instantiation), pack-expanded
   base-specifier lists `Base<Args>...`.
5. **Trait corners** (~8): __is_trivially_assignable/constructible
   truthiness gaps (600/700 static_assert failures), nothrow
   shorthand via std::is_nothrow_default_constructible member,
   __is_nothrow_invocable caching, enable_if-gated constructor
   selection in partial specializations, converting-ctor alias
   compatibility.
6. **Pack corners** (~6): __integer_pack/__type_pack_element nested
   uses, sizeof...(pack) in enable_if, multiple pack ctor params.
7. **__complex__** (2): `__complex__ long double` type +
   __builtin_complex; likely model as a two-element aggregate with
   real/imag halves (compile acceptance).
8. **Misc**: libstdcxx-uninitialized-copy parse (read the namespace
   body error context), nonprimary-embedded-class inline-var
   constexpr image, lambda invoke_result pack run test.

Next clusters, in leverage order:

1. **Would-it-compile trait family** — landed via
   SemExprAnalyzer::EvaluateSemaProbeTrait (declval surrogates through
   CopyInitialize / ResolveClassCtorHost / ResolveOperatorCall with
   SemTreeMayThrow for the nothrow variants and the ClassHasTrivial*
   facts, including the new ClassHasTrivialDefaultCtor). Pack-expanded
   trait arguments expand through the new ExpandPackTypeId host API
   (per-element scopes shared with ExpandPackExpression); a trait
   argument's ellipsis may arrive as the abstract declarator's DI_PACK
   marker. Fold-expressions parse as EK_FOLD and evaluate in the
   constant subset (&&/|| with empty-pack identities and short-circuit
   rescue); runtime folds and the remaining fold operators are a
   documented boundary. `__is_nothrow_invocable` and standard-layout
   member-position corners are still open.
2. **Statement/declaration attribute positions** (~10):
   `[[...]]` before block-scope declarations, in conditions,
   for-init, range-for; attributed null statement
   (`[[__fallthrough__]];`); GNU `__attribute__` declaration prefix
   (500-gnu-attribute-declarations); namespace prefix/suffix/alias
   attributes (`namespace X __attribute__((...)) {`, `inline namespace
   Y [[...]] {`).
3. **GNU alias keywords** (~6): `__signed`, `__const`, `__volatile`,
   `__inline`/`__inline__`, `__thread` (→ thread_local), `__decltype`,
   `__typeof__`/`typeof`, `__restrict`(ignore), `__extension__`
   (ignore), `__complex__`/`_Complex`, asm statement forms
   (`__asm__ __volatile__ (...)` with operands, statement position).
4. **Builtin function families** (compile+run suites): bswap, clz/ctz/
   popcount (+g/l/ll variants), overflow family, mem*/str* extensions
   (memchr/strchr/strcmp/memcmp/bzero), inf/nan/nans/huge families,
   expect, prefetch, assume_aligned, addressof, fpclassify,
   flt_rounds, is_constant_evaluated, operator_new/delete, complex,
   offsetof direct form, `__func__`. Extend
   SemBinder::ResolveBuiltinFunction + lowering (lower_arg_bind
   expands the inline-able ones); register each in hosted_probes.
5. **Builtin types**: `__int128`/`unsigned __int128` (+ postfix sign
   specifier forms), `__float128`/`_Float16`/`_Float128`, `_BitInt`,
   `_Atomic` specifier, block pointers (`^` declarator) + Clang
   nullability qualifiers (`_Nonnull` etc.) parsed-and-dropped,
   `__builtin_va_list`.
6. **Atomic builtin families**: `__atomic_*` (memorder tail arg),
   `__c11_atomic_*` over `_Atomic` types, `__sync_lock_*`,
   `__atomic_always/is_lock_free`; lowering to the existing atomic or
   locked instruction shapes.
7. **Language features**: structured bindings (class member
   decomposition with real reference semantics), fold expressions
   (all four forms over `&&`/`||`/`,` at least), templated lambdas
   (`[]<class T>` with/without parameter clause), deduction-guide
   declarations (parse/accept under -std=gnu++17 sidecar),
   designated initializers, `__integer_pack` /
   `__type_pack_element<I, Ts...>`, `__builtin_invoke`,
   `using ... __attribute__((using_if_exists))` missing-target
   tolerance, `__is_identifier` detection idiom.
8. **Run suite** (24): mostly builtin lowerings from cluster 4/6 plus
   hosted headers (cstdio/cstring/cmath/csignal/cassert) compiling
   through the system include chain; the PA32/33 object path links
   them with the host toolchain.

## Closing summary (stage complete)

All 272 pa34 tests and the root `make test-report-through-pa34` gate
pass. The final clusters landed as:

1. **C++17 statement forms**: `if constexpr` binds only the taken
   branch (SemBinder::BindConstexprIfStatement; the discarded branch
   never instantiates), if/switch init-statements desugar as a
   wrapping compound (the init-statement parser is shared with for
   and also accepts alias-declarations), and `explicit(expr)` rides
   AstMemberSpecifier::explicit_expr, evaluated per instantiation
   (SemBinder::ExplicitSpecifierValue); deduction guides accept the
   conditional form too.
2. **Templated lambdas**: the `[]<...>` head parses into a DK_TEMPLATE
   shell on AstLambda; an immediate invocation deduces the head from
   the analyzed call arguments (AnalyzeTemplatedLambdaInvoke reuses
   MakePatternParamScope/DeduceFixedParameter/MakeArgumentAliasScope)
   and binds the closure under the argument aliases. Lambda parameter
   default arguments ride ScopeBinding::fn_defaults. An uninvoked
   templated lambda stays a documented boundary.
3. **Structured bindings**: `auto [a, b]` (block and range-for forms)
   desugars to a hidden `__sbN` object plus per-name `auto&` bindings
   onto the members in declaration order, giving real reference
   semantics; the definition-time template check knows the introduced
   names.
4. **Designated initializers**: `.name = value` elements bind to their
   named fields with value-initialized holes in the aggregate
   consumers and in MakeAggregateTemporary (compound literals
   `(T){...}` route through the functional-cast shape); braced lists
   now also convert to aggregate parameters field-wise
   (ImplicitConversion::aggregate_list), unions select the designated
   member, and the nested-union one-member rule counts only the
   current nesting level's actions.
5. **GNU complex**: `_Complex`/`__complex__` build a synthesized
   two-field record ({__real, __imag}) bound at global scope on first
   use (SysV complex ABI matches the pair layout); __builtin_complex
   constructs it field-wise and __real__/__imag__ select parts.
   Complex arithmetic stays a boundary.
6. **Pack corners**: `__integer_pack(N)` expands to its value run in
   ExpandTemplateArgumentPack (dependent operands keep the pattern
   carry); `__type_pack_element` is a builtin alias template resolved
   through a shadow TemplateInfo (dependent uses defer like alias
   uses); special-member scopes publish the expanded pack parameter's
   umbrella binding; mem-initializer arguments expand `u...` items;
   the leading-pack sealing in DeduceFunctionTemplate keeps packs
   open when a fixed template-spec parameter can deduce them.
7. **Trait corners**: ProbeTraitInvocable implements
   __is_invocable/__is_nothrow_invocable over declval surrogates
   (class callables resolve operator() via ResolveOperatorCall);
   `__is_nothrow_invocable<...>` template-ids resolve to the builtin
   (GCC-13 behavior the refs pin) unless a namespace-scoped user
   declaration exists (the pa19 fixture's std::__is_nothrow_invocable
   keeps priority); __array_rank is a value trait;
   ProbeTraitConvertible completes pointee classes so derived-to-base
   over uninstantiated specializations answers correctly;
   __builtin_invoke expands pack arguments before dispatch and
   resolves class callables over analyzed values.
8. **Misc**: co_await/co_yield/co_return parse as contextual
   operators only inside template scopes (the pa10 negative fixture
   keeps rejecting them in plain functions; instantiation throws);
   `= {}` on an empty-aggregate constexpr variable evaluates its own
   zero image (has_explicit_init).

File-audit splits at stage close: statement binders moved to
sema/sem_stmt.cpp, aggregate-constructor synthesis (with the PA34
designated realignment) to sema/sem_aggregate.cpp, the PA34 builtin
templates and the GNU complex record to sema/sem_builtin_template.cpp,
the InstantiationContext RAII definition to sema/sem_instantiation.h,
array braced-init to sem_apply.cpp, and redeclaration signature
agreement to template_order.cpp.

## Architecture Review

How the stage's ownership boundaries came out in the implementation:

- **Hosted facts enter only at the front.** The host probe
  (`gen_builtin_host_config.pl`, regenerated per build with a compare
  guard) bakes `-dM` macros and include dirs into a generated header
  that only `toolchain/hosted_env` reads. `hosted_env` feeds the
  preprocessor via `CompileOptions`; nothing downstream of the token
  stream consults the host environment, so `--emit-lowir` stays
  representative and object emission consumes serialized LowIR only.
  The pa5 `preproc` tool and the `--emit-*` fixture modes construct
  plain unhosted `Preprocessor`s — the gate is the options struct, not
  environment sniffing.
- **One capability registry.** `hosted_probes` answers `__has_builtin`
  and friends for the preprocessor and backs the sema-side builtin
  tables. The registry under-reports (some accepted builtins answer 0)
  but never over-reports: nothing advertises a capability sema
  rejects.
- **Traits computed from the class model.** The `EK_BUILTIN_TRAIT`
  evaluator and the would-it-compile probes
  (`ProbeTraitConstructible`/`Assignable`/`Invocable`/`Convertible`)
  run declval surrogates through the real conversion/overload
  machinery with probe-scoped error containment, reading
  `ClassInfo` triviality/noexcept facts. There are no name-keyed
  canned answers. The two builtin templates
  (`__type_pack_element`, `__is_nothrow_invocable`) resolve through
  shadow `TemplateInfo`s with per-argument-key caching; the
  builtin-vs-user priority for `__is_nothrow_invocable` keys on the
  declaring scope's kind (GCC-13 behavior), not on any fixture.
- **Concessions are grammar plus AST, not text matches.** Every parser
  concession (templated lambdas, structured bindings, designated
  initializers, `__decltype`/`__typeof__`, `_Complex`, contextual
  `co_*`, GNU attributes) lands as a grammar rule building real AST
  that sema binds. Parse-and-discard is limited to the documented
  boundaries (asm bodies, attribute payloads, nullability qualifiers,
  `__restrict`, `__extension__`); everything else either binds with
  real semantics or throws a boundary error — there is no fallback
  success path.
- **Alias collapse is the one deliberate divergence.** `_FloatN` /
  `__float128` map to the nearest evaluated standard types, which can
  make distinct glibc per-format declarations collide. The audit
  scoped both resulting first-wins tolerances (duplicate explicit
  specializations, duplicate inline definitions) to landings whose
  resolution actually spelled a collapsed alias — tracked by a typed
  counter/flag chain (`collapsed_alias_uses_` →
  `ClassSpecialization::alias_collapsed` /
  `SemNode::alias_collapsed` → `LowFunctionInfo::alias_collapsed`) —
  so ordinary redefinitions still error.

## Final Architecture Review

The stage audit (pa34/audit.md) confirmed the boundaries above and
closed the gaps it found; the final state:

- **No fallback success paths.** The audit removed the three that had
  crept in: `co_return` no longer silently compiles as `return` (it
  now throws the stage-boundary error at binding, matching
  co_await/co_yield); the fold-expression constant evaluator no
  longer folds past a non-constant operand that runtime evaluation
  would have reached (position-aware short-circuit); and the
  `std::is_nothrow_*_constructible::value` shorthand only answers for
  a class template actually declared in namespace std. Every
  concession now either binds real semantics or fails loudly.
- **Standard behavior in non-template code.** Non-template
  `if constexpr` fully checks its discarded branch (binding into a
  detached holder outside return-type deduction); only template
  instantiation skips it, keyed on the binder's `instantiating_`
  state.
- **The alias-collapse tolerances are typed and scoped.** Duplicate
  explicit specializations and duplicate inline definitions are
  accepted first-wins only when a collapsed `_FloatN` spelling was
  involved on either landing, tracked through
  `collapsed_alias_uses_` → `ClassSpecialization::alias_collapsed` /
  `SemNode::alias_collapsed` → `LowFunctionInfo::alias_collapsed`.
  Plain redefinitions error again.
- **Overflow builtins follow GCC's semantics.** Operands keep their
  own promoted types and the lowering checks exact-128-bit results
  against the result type; the 128-bit cases that the scheme cannot
  check are a loud boundary instead of a silent never-overflows.
- **Hosted include search matches GCC for angle includes** (no
  including-directory and no CWD fallback in hosted mode); the
  unhosted PA5 order is untouched.
- **Parser cost is linear where it was exponential**: the paren
  primary parses the expression first and consults the fold tail only
  behind a depth-0 `...` pre-scan.
- **Perf posture**: host macro import is map-based and parsed once
  per process; builtin dispatch tables are static with a two-byte
  prefix guard on the lowering hot path; probe/trait template uses
  cache per argument key; pack expansion is O(N).
- **Remaining documented boundaries** (all loud errors or
  documented-and-harmless): complex arithmetic, inline-assembly
  codegen and asm-label renaming (PA36), uninvoked templated lambdas,
  runtime fold expressions and the non-logical fold operators,
  `_Atomic` qualification dropped outside the operator families,
  Clang block pointers composed as function pointers, 128-bit
  overflow-builtin operands/results, structured bindings without the
  tuple-like (`std::get`) protocol, and `__has_cpp_attribute`
  answering 0 while attributes stay parse-and-discard.

Exit state: fileAudit pass (0 fatal), `make test-report-through-pa34`
3189/3189 across pa1-pa34.
