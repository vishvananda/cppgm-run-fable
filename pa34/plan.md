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

Next clusters, in leverage order:

1. **Would-it-compile trait family** (~40 tests):
   `__is_constructible` / `__is_trivially_constructible` /
   `__is_nothrow_constructible` (+ pack-expanded args),
   `__is_convertible`, `__is_assignable` / nothrow / trivially,
   `__is_destructible` / trivially, `__is_pod` / `__is_trivial` /
   `__is_trivially_copyable` / `__is_standard_layout` /
   `__is_literal_type`, `__has_trivial_constructor`,
   `__reference_constructs_from_temporary` / `__reference_binds_to_
   temporary`, `__is_nothrow_invocable`, `__is_identifier`.
   Implementation route: extend sem_trait.cpp evaluation but move the
   would-it-compile probes into SemExprAnalyzer (they need
   ResolveClassCtorHost / conversion / assignment analysis wrapped in
   try/catch over synthesized declval operands, plus ClassInfo
   triviality bits). Add each name to hosted_probes kBuiltinTraits only
   when its evaluation lands.
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
