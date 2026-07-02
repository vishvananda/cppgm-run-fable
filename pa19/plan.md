# PA19 Plan: Metaprogramming Slice (`cppgm++ --emit-lowir`)

## Goal

Extend the PA18 template layer with the first compile-time
metaprogramming slice: integral non-type template parameters and
arguments, type/non-type parameter packs and pack expansions, explicit
specialization of class and function templates, integral
constant-expression template arguments, constant-valued bindings
(`static const` / `static constexpr` members, `constexpr` variables),
and `static_assert` (immediate and instantiation-deferred). Everything
lowers through the existing PA14-PA18 LowIR path; instantiation still
produces ordinary sema entities.

Additional scope demanded by the checked-in fixtures (the oracle wins
over the handout's out-of-scope list):

- a minimal class-template partial-specialization facility
  (`is_same<A, A>`, `enable_if<true, T>` from `tests/support.h`),
  selected by structural pattern matching over the existing deduction
  unifier;
- variable templates with constant values: primary, explicit
  specialization, and the same minimal partial-spec matching
  (`same_v<T, T>`, `flag<less<>>`, `datasizeof_v<T>`);
- alias templates parse-and-capture without semantic support
  (`void_t`, never instantiated by the tests);
- function-template deduction of a non-type argument from a
  class-template specialization argument position (`store<N, U>`
  against `store<0, int>`), i.e. value slots in the unifier;
- a numeric literal-operator template (`template<char... Cs> int
  operator ""_x()`), which also needs the literal-operator-id
  declarator parse;
- `sizeof...(pack)` (parser + sema).

## Architecture Recap (what we build on)

- The PA10 AST already parses every PA19 form: `AstTemplateParameter`
  (kind TP_TYPE/TP_NON_TYPE/TP_TEMPLATE, `pack`, default type/expr),
  `AstTemplateArgument` (type/expr alternative + `pack` flag),
  `template<>` heads (`has_parameter_list == false`), base-specifier
  packs, `DI_PACK` declarator items, `EK_PACK_EXPANSION` expressions,
  and `DK_STATIC_ASSERT`. Missing parses: `sizeof...(identifier)` and
  the literal-operator-id.
- PA18 template machinery (`template_info.*`, `sem_template.cpp`,
  `template_deduce.cpp`): capture -> on-demand instantiation by
  re-binding stored AST under a `SCOPE_TEMPLATE_PARAMS` alias scope;
  specializations keyed by `TemplateArgumentKey`; function templates
  deduce via structural unification (`DeduceFromType`) over positional
  placeholder types; bodies instantiate on odr-use
  (`OnSpecializationOdrUsed`).
- `const_expr.cpp` evaluates the 5.19 integral subset over the AST with
  the binder as `IConstExprContext`; `ScopeBinding.has_value` records
  constant variables and enumerators.
- The lowering demand-emits weak definitions; the pa19 comparator
  canonicalizes symbol names and drops `object=` metadata (mangling
  must be structurally sane and unique, not byte-exact).

## Design

### Template-argument model (`TemplateArg`)

The single deep change: template arguments stop being bare `TypePtr`s.

    struct TemplateArg {            // lives in sema/type.h
        TypePtr type;               // the type argument; for values,
                                    //   the declared parameter type
        bool is_value;
        ConstValue value;           // concrete integral value
        int value_param;            // pattern slot: names the enclosing
                                    //   template's value parameter (else -1)
        const AstExpr* dependent_value; // pattern slot: unevaluated
                                    //   dependent argument expression
    };

`ConstValue` moves from `scope.h` to `type.h` (it only needs
`EFundamentalType`). Concrete arguments always have
`value_param == -1 && !dependent_value`. Pattern (dependent) arguments
appear only inside `TemplateInfo::dependent_uses` / partial-spec
patterns / TK_TEMPLATE_SPEC types, never in an instantiated
specialization's identity.

Migrated carriers: `ClassSpecialization::args`,
`FunctionSpecialization::args`, `NamedTypeInfo::spec_args`, the
deduction `bound` vector, and TK_TEMPLATE_SPEC pattern types (new
`Type::targs` field used only by that kind; `Type::parameters` stays
function-only). Key/spelling helpers (`TemplateArgumentKey`,
`TemplateArgumentSpelling`) render `v<fundamental>:<bits>` /
source-like value spellings; 14.4 equivalence is (parameter type,
converted value). Walkers updated: `TypeEquals`, `TypeIsDependent`,
`DescribeType`, `AppendKey`, `DeduceFromType`,
`SubstituteOrderingTypes`, ADL association, the Itanium mangler
(`LI<type><value>E` form, negative values with `n`), and the lowering
scope-path spelling.

### Template parameters (`TemplateParam`)

Gains `kind` (TYPE/VALUE), `pack`, and for value parameters the source
`AstTemplateParameter*` (declared type composes on demand — it may be
dependent, `template<class T, T v>`) plus `default_expr`. At most one
pack per parameter list (checked); parameters after a pack must be
defaultable (or deduced) — the pack absorbs the middle run of an
explicit argument list.

### Argument resolution

`ResolveTemplateArgumentList` becomes parameter-driven over the
flattened argument list:

1. Source arguments marked `pack` expand first (see Packs).
2. Leading non-pack parameters consume arguments 1:1; a pack consumes
   the remaining run; parameters after the pack fill from defaults.
3. Type parameter: argument must resolve as a type.
4. Value parameter: compose the declared parameter type under the
   earlier bindings (partial alias scope); an argument that parsed as
   a type-id re-reads as an id-expression when it is a plain (possibly
   qualified) name (the parser prefers the type reading for `Box<A>`);
   evaluate with `EvaluateConstExpr` in the *use* scope; convert with
   `ConvertConstValue` to the parameter type (enum parameters convert
   to their underlying type; the arg's `type` keeps the enum identity
   for spelling/mangling). An evaluation that touches names bound in a
   `SCOPE_TEMPLATE_PARAMS` scope without concrete values produces a
   dependent pattern argument instead (usable only in pattern
   contexts; instantiation re-resolves it).
5. Defaults: as PA18, extended to value defaults evaluated under the
   partial scope.

### Value parameters in scope

`MakeArgumentAliasScope` binds a value parameter as `SB_VARIABLE` with
`has_value = true` and a new `no_object` flag (const-qualified declared
type). Expression analysis folds any rvalue read of a `has_value`
binding to a constant (see next section), so `return N;` lowers as an
immediate and `LookupConstant` already accepts it in constant
expressions.

### Constant bindings and folding

- Class-scope `static const` / `static constexpr` integral members
  with constant initializers record `has_value` on the member binding
  at class binding time (already true at namespace scope), including
  inside instantiated specializations (the initializer evaluates under
  the alias scope) and through base-class qualified lookup.
- 3.2p2-style folding: an id-expression or member read of a
  `has_value` binding in an rvalue context produces a constant prvalue
  (no load, no `declare global`); lvalue contexts (address-of,
  reference binding) keep the entity reference. This also fixes the
  pre-existing `100-dependent-static-constant-member-comparison`
  LowIR mismatch.
- Out-of-class static-data-member template definitions keep PA18's
  registration; their emitted globals are weak/demand-emitted, so
  folding naturally yields the fixtures' "no storage" shape.
- `constexpr` variables with function-style paren initializers record
  constants (`constexpr error_type e(v);`).

### static_assert

- Namespace/class/block scope evaluate immediately (block scope: via
  the statement path in analyzed bodies — including never-called
  member bodies, which the fixtures require to fail).
- Inside template patterns the declaration is skipped at definition
  time (the pattern body is never analyzed — already true) and
  evaluates naturally at instantiation when the class/function body
  re-binds. A condition whose evaluation is dependent at definition
  time must not error; the sanity walk stays silent on them.

### Packs

Representation: a pack parameter's binding in the alias scope is a
pack alias (`ScopeBinding.is_pack` + `pack_args` vector of
`TemplateArg`); a function parameter pack binds as a pack of expanded
parameter bindings (`pack_param_names`). `sizeof...(name)` reads the
element count of either kind. Specialization identity stays the
flattened `TemplateArg` vector.

Expansion mechanism (mirrors the PA18 re-binding philosophy): each
expansion site iterates the pack elements, temporarily re-binding the
pack name to element `i` in a transient child scope, and re-runs the
ordinary resolution on the stored pattern AST:

- template-argument lists: `tuple<T...>`, nested patterns
  (`integral_constant<bool, bool(Bn::value)>...`);
- base-specifier packs: `: store<T>...`, `: data<T...>...` during
  instantiation-time base-clause binding;
- parameter clauses: `Args... args`, `Args&&... args`, unnamed
  `T&&...` — signature composition expands the clause per element and
  binds `args` as a parameter pack;
- call arguments / braced-init lists: `EK_PACK_EXPANSION` items expand
  per element (`h(args...)`, `T(declval<Args>()...)`,
  `{args...}`, `{I..., 0}`); lockstep expansion when the pattern
  mentions both a type pack and an expression pack
  (`static_cast<Args&&>(args)...`).

Deduction with a trailing pack: leading fixed parameters unify as
today; each remaining call argument deduces one pack element (decayed;
forwarding-reference elements deduce lvalue references). Explicit
argument lists bind leading parameters, then the pack absorbs the
rest. `Box<Args...>::f` out-of-class member definitions positionally
rebind the pack.

Numeric literal-operator template: parse `operator "" identifier`
declarator-ids; a user-defined integer literal without a matching
ordinary operator instantiates the template with `char...` arguments
spelled from the literal's source characters.

### Explicit specialization

`template<>` (`has_parameter_list == false`):

- Class (`template<> struct Box<int> {...}`): resolve the primary
  template + concrete args; claim/insert the `ClassSpecialization`
  slot with `explicit_spec` marking; bind the provided definition as
  an ordinary class completing the slot's entity (no alias scope; the
  injected name maps the template-id to the entity). A stale implicit
  instantiation that was already created (declared uses only) refreshes
  when the fixtures require it; uses after the spec see the explicit
  body. Declaration-only specializations of declared-only primaries
  (`template<> struct trait<signed char> {};`) work without a primary
  definition.
- Function (`template<> int f<int>() {...}` and the deduced form
  `template<> inline int digits(unsigned int)`): resolve args
  explicitly or by deducing the pattern against the declared
  signature; register a `FunctionSpecialization` whose body AST is the
  explicit definition (`spec_decl`); body instantiation binds that AST
  (no parameter aliasing beyond the template args); an
  already-emitted primary body for the same key is replaced only in
  the not-yet-emitted state (14.7.3p6 use-before-visible is UB —
  fixtures only exercise the declared-after-use-free orders).
- Variable (`template<> inline const bool flag<less<>> = true;`):
  handled by the variable-template registry.

### Class partial specialization (minimal, fixture-driven)

`TemplateInfo` gains `partial_specs`: each records its own parameter
list and pattern `TemplateArg` list (composed abstractly over
placeholders) plus the definition AST. At `EnsureClassSpecialization`
time, patterns match against the concrete argument list via
`DeduceFromType` extended to `TemplateArg` (value slots must match
exactly; repeated parameters must unify consistently — `is_same<A,A>`).
Most specialized wins among matches when unambiguous by the existing
partial-ordering subset; the fixtures only need exact-match vs primary
selection. The instantiation binds the partial spec's AST under its
own deduced alias scope.

### Variable templates

New `TMPL_VARIABLE` capture (primary + explicit + partial specs, same
matching machinery). Uses (`name<args>` id-expressions) resolve to a
constant `ScopeBinding` (`has_value`) whose value evaluates from the
selected initializer under the alias scope on first use, cached per
argument key. Reads fold like other constants (rvalue contexts only);
the fixtures never odr-use one as an object.

### Ownership boundaries (file audit)

- `sema/template_args.cpp` (new): TemplateArg helpers, key/spelling
  value rendering, argument-list resolution.
- `sema/sem_pack.cpp` (new): pack binding, expansion drivers,
  `sizeof...` semantics.
- `sema/sem_spec.cpp` (new): explicit/partial specialization capture +
  selection, variable templates.
- Existing files keep their PA18 roles; `sem_template.cpp` stays the
  instantiation driver. New files register in
  `dev/frontend_source_sets.mk` (cppgm++ set).

## Validation plan

- Fast loop: `make test-report ACTIVE_TEST_REPORT_PAS='pa19'`; single
  test: `make -C pa19 check TEST=tests/spec/....t`.
- Gate after each meaningful shared-infrastructure change:
  `make test-report-through-pa19` (PA11 dump behavior, PA12-17
  overload resolution, and all PA18 template tests must stay green).
- `perl scripts/cppgm_file_audit.pl --stage pa19 --paths dev/src`
  after new files land.
- Implementation order:
  1. `TemplateArg` model migration (no behavior change; through-pa18
     stays green).
  2. Non-type parameters/arguments + constant bindings + folding +
     deferred static_assert (~55 tests).
  3. Constant-expression extensions (string-literal subscript,
     function-style constexpr init, sizeof type-fallback).
  4. Packs + `sizeof...` + literal-operator template (~18 tests).
  5. Explicit/partial specialization + variable templates (~14 tests).
  6. Stragglers (stale-instantiation fixture, LowIR diffs), full gate,
     file audit.

## Status

- [x] Architecture mapped; failing tests categorized (51 non-type, 15
      pack, 10 explicit-spec, 2 partial-spec, 2 variable-template, 3
      parse, 3 const-expr, 3 LowIR-shape).
- [ ] Stage 1: TemplateArg model migration (through-pa18 stays green).
- [ ] Stage 2: non-type parameters/arguments end to end.
- [ ] Stage 3: const-expr extensions.
- [ ] Stage 4: packs + `sizeof...` + literal-operator template.
- [ ] Stage 5: explicit/partial specialization + variable templates.
- [ ] Stage 6: stragglers, full through-pa19 gate, file audit.

### Fixture-derived notes

- `100-dependent-static-constant-member-comparison`: reads of
  recorded-constant static members must fold (no `declare global`, no
  load) — the general 3.2p2 rule, not a test-shape special case.
- `400-stale-function-template-instantiation-lookup`: currently
  compiles with a diverging shape (the ref emits a ctor-style call
  for `node<int> n{}` that we skip); diagnose during stage 6 against
  the full ref before changing lowering.
- Value-argument spelling in entity names uses the canonical decimal
  value (`Box<3>`), so distinct values cannot collide; enum-typed
  values spell through the enum's qualified enumerator when one
  matches, else the underlying decimal.
