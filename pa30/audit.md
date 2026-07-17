# PA30 Audit: `abimangle`

## Audit Plan

Scope: the PA30 implementation commit (`bf6b359a6`) against `pa30/plan.md`,
`pa30/README.md`, and `doc/itanium-mangling.txt`.

Files to inspect:

- `dev/abimangle.cpp` — CLI driver; must stay starter-kit shaped (no logic).
- `dev/src/abi_mangle.h` — handout scaffold; must remain unmodified.
- `dev/src/abi/abi_fact_parse.cpp` — fact-file parsing; strictness of integer,
  discriminator, and flag words; loud rejection of malformed facts.
- `dev/src/abi/abi_fact_serialize.cpp` — canonical inverse of the parser;
  round-trip preservation for every type spelling.
- `dev/src/abi/abi_mangle_encode.cpp` — type/template-arg/expression encoding,
  substitution-table ordering and key unification.
- `dev/src/abi/abi_mangle_names.cpp` — qualified names, function encodings,
  operator terminals, local names, special names, per-case driver.
- `pa30/Makefile`, `pa30/scripts/`, `pa30/tests/` — confirm handout
  provenance; no harness weakening or fixture tampering.
- `dev/frontend_source_sets.mk` — build wiring; no impact on other stages.

Performance risks to check: substitution-table lookups, recursive
`type_key`/`argument_key` recomputation on nested facts, per-line word
splitting, and file assembly (`mangle_fact_files`) — all against the actual
input scale (fact cases of tens of lines).

Ownership boundaries to check: parser owns text→facts, serializer owns
facts→text, `abi_mangle_encode.cpp` owns type/arg/expr encoding plus the
substitution table, `abi_mangle_names.cpp` owns name assembly and target
dispatch; substitution identity must stay structural (canonical keys), with
no downstream reparsing of assembled strings.

File-audit issues to check: `perl scripts/cppgm_file_audit.pl --stage pa30
--paths dev/src` clean; no hidden fragments, oversized files, or code moved
to unchecked paths.

Cheating checks: no reference binary, host compiler, or demangler invoked;
no test-name or source-shape gates; no embedded fixture payloads; failure
paths exit nonzero rather than emitting fallback names.

## Findings

Confirmed-good (no change needed):

- Handout integrity: `dev/src/abi_mangle.h`, `pa30/tests/`, `pa30/scripts/`,
  and `pa30/README.md` match the assignment exports; the only `pa30/Makefile`
  change routes `ref-test` through the shared handout include
  (`scripts/pa_run_check_targets.mk`) since PA30 has no reference binary.
- No skipped phases or fallback success paths: every unknown keyword, type
  head, operator, terminal, or dangling reference throws `AbiFactError`, and
  the driver converts any exception into a nonzero exit.
- Substitution is structural: one `SubstitutionTable` per name, canonical
  keys unify parsed type structure with the explicit keys the fact files
  carry (verified against the `RKS_`, `NS_4makeEv`, member-template
  terminal-slot, and `template-param-subst` fixtures).
- Ownership boundaries match the plan: parse / serialize / encode / names are
  separate translation units behind one internal header; the driver holds no
  encoding logic.
- Performance is proportionate: per-case maps are O(log n) on tiny n; the
  only super-linear shape is `type_key` recomputation on nested type chains,
  bounded by the 64-deep resolution guard and centimeter-scale inputs — no
  quadratic full-suite scans, no hot-path recomputation worth caching.

Defects found (all fixed in this audit):

1. **Non-member unary operator arity miscount** (`abi_mangle_names.cpp`,
   `place_terminal`). `is_member` treated a global operator terminal slot
   (`function path operator`) as a member, inflating the operand count by
   one: a global unary `operator-` with one parameter would encode as binary
   `mi` instead of unary `ng`. Member-ness now comes only from the presence
   of prefix components.
2. **Plain entity template arguments wrapped in `X…E`**
   (`abi_mangle_encode.cpp`, `encode_entity_address`). A non-address-of
   entity argument emitted `XL_Z…EE`; per the Itanium grammar
   (`<template-arg> ::= <expr-primary>`) an external-name literal is spelled
   directly as `L_Z…E`. The `X…E` wrapper now applies only to the `ad`
   (address-of) form.
3. **Missing two-digit discriminator form** (`abi_mangle_names.cpp`,
   `local_discriminator`). Occurrence numbers whose Itanium discriminator is
   ≥ 10 must use `__<n>_`, not `_<n>` (itanium-mangling.txt §local-name).
4. **Serializer dropped `volatile` on combined cv facts**
   (`abi_fact_serialize.cpp`, `serialize_type`). A colon-form
   `const:volatile:` layer parses into one cv node with both flags; the
   space-form serialization emitted only `const`, silently changing the
   type. Both qualifiers are now emitted.
5. **Serializer emitted unparseable raw array bounds**
   (`abi_fact_serialize.cpp`, `serialize_type_word`). An unknown-bound array
   serialized as `array::elem`, which re-parses as a single opaque name; the
   empty bound now round-trips as `array:-:elem`.
6. **`construction-vtable` accepted negative offsets silently**
   (`abi_fact_parse.cpp`). A negative base offset wrapped through the cast
   to `unsigned long long`; offsets are now parsed as non-negative indices.
7. **Occurrence/discriminator words were never validated**
   (`abi_fact_parse.cpp`). Local-type, lambda-closure, and local/lambda
   context discriminators flowed as raw strings into `strtoll`, so garbage
   silently meant "no discriminator". They are now required to be `-` or an
   unsigned decimal at parse time.
8. **19-digit integer overflow was silent** (`abi_fact_parse.cpp`,
   `parse_integer`). `strtoll` saturation to `LLONG_MAX`/`LLONG_MIN` went
   undetected; out-of-range facts now fail loudly via `errno` checking.
9. **Duplicate definition ids silently overwrote earlier facts**
   (`abi_mangle_names.cpp`, `build_case_environment`). Within one case, a
   repeated `let-*` id replaced the prior record (last-wins); duplicates are
   now an error, keeping one owner per fact id.
10. **Dead `ctx` parameter on `place_terminal`** (`abi_mangle_names.cpp`).
    Removed.

## Changes Made

- `dev/src/abi/abi_mangle_names.cpp`: member-ness from prefix links only;
  `__<n>_` discriminator form; duplicate-id rejection in
  `build_case_environment`; `place_terminal` signature trimmed; strict
  digit parsing in `local_discriminator`.
- `dev/src/abi/abi_mangle_encode.cpp`: direct `L…E` expr-primary spelling
  for non-address entity template arguments.
- `dev/src/abi/abi_fact_serialize.cpp`: cv layers emit both qualifiers;
  raw array bounds round-trip as `-`.
- `dev/src/abi/abi_fact_parse.cpp`: occurrence-word validation
  (`parse_occurrence_word`) applied to all six discriminator positions;
  `parse_integer` overflow detection; non-negative construction-vtable
  offsets.
- `pa30/plan.md`: added Architecture Review and Final Architecture Review.

## Validation

- `make -C pa30 test` — 73/73 ABI fixtures pass after the fixes.
- Round-trip check (bring-up harness, not committed):
  `parse → serialize → parse → serialize/mangle` stable across every
  checked-in fixture plus synthetic cases for the previously broken
  spellings (`ptr:const:volatile:int` → `PVKi`, `ptr:array:-:char` →
  `PA_c`).
- Behavior spot checks through the built binary: global unary
  `operator-` → `_Zng1C` (was `mi`), member `C::operator-` → `_ZN1CmiE1D`,
  plain entity argument → `6HolderIL_ZN2ns1vEEE` (was `X…E`-wrapped),
  occurrence 13 → discriminator `__11_`, duplicate `let-type` id and a
  non-numeric discriminator both exit nonzero with diagnostics.
- `perl scripts/cppgm_file_audit.pl --stage pa30 --paths dev/src` — pass.
- `make test-report-through-pa30` — 2755/2755 tests, 30/30 stages pass.
- `git status --short` — clean after the audit commit.
