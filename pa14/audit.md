# PA14 Audit

## Audit Plan

Scope: commit 64ea0f018 (PA14 emit-lowir) against `pa14/README.md`,
`pa14/plan.md`, and the PA1-PA13 regression surface.

Files to inspect:

- `dev/src/lowering/lower_unit.cpp`, `lower_program.h`: entity registry
  identity keys, declare/define emission, registry mutation during
  rendering (reference stability, demand-driven entries).
- `dev/src/lowering/lower_function.{h,cpp}`: block/slot/label state,
  statement lowering, switch label pre-scan, goto maps.
- `dev/src/lowering/lower_expr.cpp`: conversion spelling contexts, call
  lowering, short-circuit shapes, pointer arithmetic.
- `dev/src/lowering/lower_name.cpp`: symbol naming, Itanium mangling
  (where the qualified path facts come from).
- `dev/src/lowering/lower_types.cpp`, `lower_const.{h,cpp}`: type
  spelling, constant folding of global initializers.
- `dev/src/sema/` diffs (sem_expr, sem_binder, decl_binder, scope.h,
  sem_node.h, type_builder, sem_convert): PA11/PA12 behavior
  preservation, default arguments, deleted functions, extern "C".
- `dev/cppgm++.cpp`: `--emit-lowir` drives the real pipeline (no
  fallback/unimplemented paths left).
- `dev/src/text_literals.cpp`, `dev/src/ast/` diffs: numeric escapes,
  postfix suffixes on keyword casts, labeled declaration statements.

Performance risks to check:

- per-call/per-node quadratic scans in the entity registry and overload
  index computation;
- repeated `FindOwnBinding` lookups in hot lowering paths;
- string-concatenation rendering costs;
- per-call default-argument and min-arity recomputation.

Ownership boundaries to check:

- the lowering must consume typed semantic facts, not re-derive them
  from printed/dump text;
- entity identity must be structural (scope object + name), not a
  re-parsed string;
- `SemNode` lowering facts must stay dump-invisible (PA12 fixtures
  byte-stable).

File-audit issues to check:

- `perl scripts/cppgm_file_audit.pl --stage pa14 --paths dev/src`;
- no hidden implementation fragments or code moved to unchecked paths;
- new lowering sources registered in `dev/frontend_source_sets.mk`.

Cheating-pattern checks:

- no test-name/fixture/source-shape gates (grep for getenv, test paths,
  ref files in `dev/`);
- `--emit-lowir` runs parse + bind + lower for real (no embedded
  payloads, no interpreter/VM substitutes; output is per-input LowIR);
- failure tests fail through real semantic errors, not blanket gates.

## Findings

1. Stringly mangling (fixed). `MangleType` in
   `dev/src/lowering/lower_name.cpp` recovered an enum/class type's
   qualified path by parsing `NamedTypeInfo::display` ("enum class
   N::E"): take the text after the last space, split on "::". The
   binder knows the declaring scope structurally when it creates the
   entity (`SemBinder::TypeDisplayName` builds the display *from* the
   scope chain), so the lowering was re-deriving a fact downstream from
   a printed spelling — exactly the ownership inversion the plan
   forbids ("never the printed dump text").
2. Ambiguous entity identity keys (fixed). `QualifiedKey` in
   `dev/src/lowering/lower_unit.cpp` keyed cross-translation-unit
   entity identity on `LowerScopePath(scope) + name`, the "__"-joined
   display path. Two distinct entities could collide: `a::b::x` vs
   `a__b::x` (identifiers may contain "__"), and unnamed-namespace
   entities of *different* translation units (whose components are
   skipped by the path) merged into one registry entry even though they
   are distinct internal-linkage objects. Tests did not exercise the
   collisions, but the registry's identity invariant was wrong.
3. Registry reference stability (fixed). `globals_` and `functions_`
   were `vector`s, while lowering hands out long-lived references into
   them: `FunctionLowerer` holds `const LowFunctionInfo&` for a whole
   body while `FunctionRef`/`RegisterGlobal` can append demand-driven
   entries (e.g. block-scope extern declarations) to the same
   containers, and `RenderGlobal(const LowGlobalInfo&)` can append via
   address-constant initializers. A reallocation would dangle those
   references. Relatedly, `Write` rendered global definitions into a
   parallel `global_texts` vector sized *before* function lowering, so
   entries registered during body lowering were not covered by it.
4. Verified clean (no change needed):
   - no test-name/source-shape/environment gates anywhere in
     `dev/src/lowering/` or the driver; unsupported constructs throw
     (EXIT_FAILURE), never silently succeed;
   - `--emit-lowir` runs the full parse + bind + lower pipeline per
     translation unit and `AddUnit` rejects synthesized class-helper
     output, so no dummy/templated/embedded-payload substitutes exist;
   - failure tests (`100-bad-*`) fail through real semantic errors;
   - registry lookups are map-keyed, the switch-label pre-scan is one
     walk of the switch subtree, the Itanium substitution table is
     per-mangling and tiny: no program-scale quadratic hot paths;
   - `SemNode` lowering facts are dump-invisible; PA11/PA12 fixtures
     stay byte-stable (full suite green);
   - default-argument min-arity is computed per call from per-overload
     recorded facts (`ScopeBinding::fn_defaults`), matching 8.3.6p9
     call-site evaluation, with candidate order [type, overloads...]
     matching the fact indexing;
   - the pa14 harness, refs, and Makefile are untouched course exports.

## Changes Made

- `dev/src/sema/type.h`, `scope.{h,cpp}`: `NamedTypeInfo` records its
  structural identity (`scope` = declaring scope, `name` = bare
  declared name) next to the display spelling;
  `TypesModel::CreateNamedTypeInfo` takes and stores both.
- `dev/src/sema/decl_binder.cpp`: the four entity-creation sites
  (class, class forward, enum, template parameter) pass the declaring
  scope and bare name.
- `dev/src/lowering/lower_name.cpp`: `MangleType` walks
  `EntityComponents` (named namespace/class scopes from
  `NamedTypeInfo::scope`, plus `name`) instead of parsing `display`;
  substitution keys stay name-based so the same entity declared in two
  translation units compresses alike. Added `LowerScopeKey`: identity
  path joined with "::" (no identifier collision), unnamed namespaces
  keyed by their per-unit scope object.
- `dev/src/lowering/lower_unit.cpp`: `QualifiedKey` uses
  `LowerScopeKey`; rendered global definitions live on
  `LowGlobalInfo::init_text` (parallel `global_texts` vector removed).
- `dev/src/lowering/lower_program.h`: `globals_`/`functions_`/
  `strings_` are `deque`s so handed-out references survive
  demand-driven growth; `LowGlobalInfo` gains `init_text`.

## Validation

- `make -C pa14 test`: 68/68.
- `make test-report-through-pa14`: 852/852 (PA1-PA13 fixtures
  byte-stable; the structural-identity and key changes are
  output-invariant for every checked-in case).
- `perl scripts/cppgm_file_audit.pl --stage pa14 --paths dev/src`:
  pass (one pre-existing PA6-era warning about
  `dev/src/parse/parser.h`, unrelated to PA14).
