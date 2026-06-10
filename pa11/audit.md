# PA11 Audit

## Audit Plan

Scope: commit 431d667a9 (PA11 emit-types) against `pa11/README.md`,
`pa11/plan.md`, and the PA1-PA10 regression surface.

Files to inspect:

- `dev/cppgm++.cpp` — `--emit-types` wiring; confirm no fallback success
  path, no test-name sniffing, failure exits, and that the PA10
  `--emit-ast` path is unchanged in behavior.
- `dev/src/sema/decl_binder.{h,cpp}` (962 lines, largest new file) —
  declaration semantics; check for stubbed declaration kinds that
  silently print nothing instead of throwing, duplicated ownership with
  `type_builder`, and stringly-typed entity facts.
- `dev/src/sema/scope.{h,cpp}` — model arena and printer; check the
  printer derives output from the model only (no source text echo), and
  binding/scope ordering is structural, not sorted-by-string.
- `dev/src/sema/scope_lookup.{h,cpp}` — 3.4.1/3.4.3.2; check the
  using-directive closure is anchored per 7.3.4 (not a flat union), and
  look for quadratic closure recomputation per lookup.
- `dev/src/sema/type_builder.{h,cpp}` — specifier/declarator
  composition; check canonical types are built structurally (TypePtr),
  not by string concatenation, and cv/array/function composition reuses
  the shared `sema/type` constructors.
- `dev/src/sema/const_expr.{h,cpp}` — constant-expression subset; check
  evaluation is over the AST (no re-lexing), UB/overflow handling, and
  that unsupported forms throw rather than yield a dummy value.
- `dev/src/sema/type.{h,cpp}` + `dev/src/sema/decl_parser.{h,cpp}` —
  shared-file changes; the PA7/PA8 regression surface
  (nsdecl/nsinit). Verify the moved 7.1.6.2p3 table is delegated to,
  not duplicated.
- `dev/src/ast/*` — `begin_token`/`end_token`/`enum_body` additions;
  PA10 dump must be unaffected; spans must be real token indices, not
  printer-driven strings.

Performance risks to check:

- per-lookup recomputation of the using-directive transitive closure;
- class layout recomputation on every `sizeof` (should be computed once
  at completion);
- repeated linear scans of scope bindings during redeclaration matching
  in large scopes;
- per-unit model teardown cost and any full-AST re-walks in the
  printer.

Ownership boundaries to verify:

- type identity lives in `sema/type` + `NamedTypeInfo` (pointer
  equality), not in spelled strings compared downstream;
- constant values are recorded on bindings at declaration time, not
  re-derived by the printer;
- `const_expr` sees the binder only through its context interface (no
  cycle);
- the printer consumes the scope model only.

File-audit issues to inspect:

- `perl scripts/cppgm_file_audit.pl --stage pa11 --paths dev/src`
  passes with no size waivers;
- no implementation fragments hidden in headers or `.inc` files to
  dodge per-file limits;
- no dead PA7-era code left behind after the 7.1.6.2p3 table move.

Cheating checks:

- run the full suite and diff `.my` vs `.ref` provenance (outputs must
  come from the binder, not copied fixtures);
- grep for test-path/fixture-name conditionals, `getenv`, hardcoded
  dump fragments;
- confirm failing tests fail through real semantic errors, not a
  blanket catch that guesses the expected exit status.

## Findings

Clean (inspected, no action needed):

- No cheating surface: `--emit-types` output is derived entirely from
  the bound scope model (`PrintTypesOutput` walks `Scope` only); no
  fixture text, test-path conditionals, `getenv`, or hardcoded dump
  fragments anywhere in the new sources; failures propagate as
  exceptions to a single `EXIT_FAILURE` path in `main`. The harness,
  fixtures, and grammar are untouched since the assignment export.
- No regressions in shared files: `DeclParser::ParseError` already
  returned `runtime_error`, so the 7.1.6.2p3 table move is
  behavior-identical for nsdecl/nsinit; the AST span/`enum_body`
  fields are ignored by the PA10 printer.
- No performance smells: per-scope `binding_index` maps make
  binding/redeclaration matching O(log n); the 7.3.4 directive closure
  is per-lookup and proportional to the lexical chain plus active
  directives; class layout is computed once at completion and reused
  by every later `sizeof` through `NamedTypeInfo`.
- `const_expr` literal evaluation re-analyzes the token spelling with
  the shared PA2 analyzers; no earlier representation exists to reuse
  (parse tokens keep only spellings), and the analysis is per-token
  O(length). Acceptable.
- File audit: the only warning (`dev/src/parse/parser.h` bad-division)
  predates PA11 (PA6-era file, unchanged by this assignment).

Defects found and fixed:

1. Ownership/stringly fact: enum scoped-ness, underlying type, and
   definedness lived in `DeclBinder::enum_facts_` (a side map keyed by
   `NamedTypeInfo*`) and were destroyed with the binder; the surviving
   model represented scoped-ness only as the `"enum class "` display
   prefix, which PA12 would have had to parse back out of a string.
2. Lookup correctness: `MergeFound` compared binding addresses, so the
   same entity found through two paths (a namespace beside its alias,
   a using-declaration import, same-type typedefs in two namespaces)
   was wrongly diagnosed as ambiguous under 7.3.4p6/3.4.3.2p3.
3. Fallback completion: `BindClass` set `complete = true` before
   computing layout, so a by-value member of the class itself
   (`struct C { C c; };`) matched the dependent-layout escape hatch
   (`complete && alignment == 0`) and the ill-formed class completed
   silently with no layout instead of failing as incomplete (9.2p9).
4. Silent wrong values: enumerator values outside the underlying type
   wrapped silently, for both explicit initializers
   (`enum E : char { a = 1000 };` dumped `a ... -24`) and the implicit
   increment at the type's maximum (7.2p5 requires representability).
5. Over-permissive merging: redeclaration merging ignored scope kind,
   silently merging duplicate class members (`int m; int m;`),
   duplicate block locals, in-class redefinition after declaration,
   and parameter redeclarations in the outermost block (3.3.3p2), and
   `BindTypeAlias` rejected the legal `typedef struct C C;`
   redefinition form (7.1.3p3-p4).

## Changes Made

- `dev/src/sema/type.h`: `NamedTypeInfo` gains `is_scoped`,
  `is_defined`, and `enum_underlying` - enum entity facts as typed
  fields of the surviving model.
- `dev/src/sema/decl_binder.{h,cpp}`: deleted `EnumFacts` and the side
  map (`DeclareEnumEntity`/`BindEnum`/`BindEnumerators` read and write
  `NamedTypeInfo`); added `SameIntegerValue`/`SuccessorValue`
  representability checks for enumerator values; `BindClass` computes
  layout before marking the entity complete; variable/function/alias
  redeclaration merging is now scope-kind aware (namespace scope,
  extern-in-block for objects, block scope for function declarations,
  the 7.1.3p3-p4 typedef redefinition rules including `typedef
  struct C C;`); `BindVariable` takes the specifier info instead of a
  lone `is_static` flag.
- `dev/src/sema/scope_lookup.cpp`: `SameFoundEntity` implements the
  7.3.4p6 same-entity rule (namespace targets, type equality for
  type names, shared type node + value for imported value bindings);
  `MergeFound` only diagnoses ambiguity across distinct entities.
- `dev/src/sema/scope.cpp`: `AddBinding` rejects a name that
  redeclares a parameter in the outermost block of a function
  definition (3.3.3p2).

## Validation

- Targeted probes (rejected: self-referential by-value member,
  parameter shadow in outermost block, duplicate member, duplicate
  local, in-class function redefinition path, `enum E : char
  { a = 1000 }`, `enum E : bool` increment overflow; accepted:
  extern-in-block redeclaration, namespace redeclarations,
  `typedef struct C C;`, `enum { a = -1, b }` with `b == 0`,
  namespace-beside-alias lookup, same-type typedefs in two
  namespaces; still ambiguous: distinct classes from two namespaces).
- `make test-report-through-pa11`: 568/568 (all stages, including the
  pa11 local suite 49/49).
- `perl scripts/cppgm_file_audit.pl --stage pa11 --paths dev/src`:
  pass (one pre-existing PA6-era warning, file unchanged by PA11).
