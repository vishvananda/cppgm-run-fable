# PA30 Plan: `abimangle`

## Goal

Standalone tool `abimangle -o <out> <fact-file>...` that reads normalized ABI
fact files and emits one Itanium-mangled name per case, in input order. No C++
parsing; the input is a typed ABI entity graph.

## Design

Three concerns, kept in separate modules (scaffold shape from
`dev/src/abi_mangle.h`, which stays the public data model):

1. **Fact parsing** (`dev/src/abi/abi_fact_parse.cpp`)
   - Line-oriented. `case <label>` opens a new case; blank lines are
     separators; every other line is one `AbiFactRecord`.
   - Word split preserves empty fields (`name-source  -` has an empty name).
   - Type specs come in two spellings, parsed into one `AbiType` tree:
     - colon form (`ptr:const:int`, `ref:array:2:char`, `named:ns::C`,
       `memberptr:ns::C:int`) — `::` is kept inside segments, single `:`
       separates;
     - space form (`template-param-subst 0`, `template std::allocator A`,
       `vendor _Atomic ulong`, `member Owner name`, `local-type ctx X 0`,
       `lambda-closure ctx 0 …`, `decltype E`, `const T`, `ref T`, …).
     A bare word is a builtin or a `let-type` reference, resolved at encode
     time (`ABI_TYPE_NAME_OR_REFERENCE`).
   - `let-arg`, `let-expr`, `let-context`, `let-entity` map onto the scaffold
     structs; cross-references stay symbolic (`*_refs`) and are resolved
     against the per-case environment during encoding.
   - Targets: `type`, `function` (simple/path/encoding/local/lambda),
     `c-function`, `variable`, `typeinfo`, `vtable`, `vtt`,
     `construction-vtable`, `tls-wrapper variable`, `thunk`,
     `virtual-base-thunk`. Function detail lines (`param`, `result`,
     `variadic`, `abi-tag`, `function-qualifier`, `name-source`, `name-std`,
     `name-template`, `function-template-arg`, `function-template-prefix`,
     `local-context`, `lambda-context`, `terminal*`, `operator-terminal`,
     `conversion-terminal`) become `AbiFunctionRecord`s in case order.

2. **Serialization** (`dev/src/abi/abi_fact_serialize.cpp`) — inverse of the
   parser in canonical (space-form) spelling; `parse(serialize(f))` is
   semantics-preserving. Used for round-trip validation of the typed model.

3. **Encoding** (`dev/src/abi/abi_mangle_encode.cpp` for types / template
   args / expressions, `dev/src/abi/abi_mangle_names.cpp` for names,
   function encodings, targets, and the per-case driver; shared internal
   declarations in `dev/src/abi/abi_mangle_encode.h`).

## Substitution model

Substitution is structural, not textual. One `SubstitutionTable` per mangled
name maps **canonical keys → seq ids** (`S_`, `S0_`, … base-36).

- Keys for facts with explicit identity come from the fact file itself:
  `name-source <name> <key>`, `name-template … <prefix-key> <complete-key>`,
  `function-template-prefix <key>`, `member-template-entity … <key>`.
- Structural keys are derived so that equal structure gives equal keys, and so
  that named-type keys unify with the explicit spellings the generator uses
  (a named type's key is its qualified name, e.g. `AbiTagBuffer`, so
  `param ref:const:named:AbiTagBuffer` hits the entry made by
  `name-source AbiTagBuffer AbiTagBuffer`).
- Entry rules follow the Itanium grammar: every prefix / template-prefix
  level, template-ids, compound types (each pointer/ref/cv/array layer),
  vendor-qualified and `u`-transform types, decltype types, substitutable
  template parameters (`template-param-subst` only — plain `template-param`
  neither reads nor writes the table, matching the fixtures where repeated
  `T_` stays spelled out), and local/closure types. Builtins and the `St`/`Sa`
  abbreviations themselves never enter. `L_Z…E` entity literals are
  self-contained encodings with a fresh table.
- Qualified-name emission searches the longest already-known prefix, emits the
  `S` code, spells the remainder, and enters each newly completed level —
  this reproduces e.g. `NS4_IS6_EE` (template-prefix substitution with fresh
  arguments) and `RKS_`/`RS5_` whole-type hits.
- Member-template template-arguments reproduce the reference behavior: the
  owner's prefix-with-arguments level is not entered; the full member-template
  key is looked up/entered at the terminal-name slot (so a second mention
  encodes as `N S<owner-prefix> I <args> E S<full> E`).

## Encoding rules pinned by fixtures

- Nested `N…E` iff more than one component after `St` merging or any
  function cv/ref-qualifier; `St<name>` stays unscoped for a single trailing
  component; `std` alone is never a table entry.
- Function templates: template-name (with abi tags) enters the table before
  the arguments are encoded; result type is emitted exactly when a `result`
  fact is present, before parameters; empty parameter list emits `v`;
  `variadic` appends `z`.
- ABI tags attach to the unqualified name as `B<len><tag>` (also on `C1`/`cl`
  terminals).
- Ctor/dtor terminals: `constructor-complete/base/allocating` → `C1/C2/C3`,
  `destructor-deleting/complete/base` → `D0/D1/D2`, filling the empty
  `name-source` slot.
- Operator terminals are semantic names; unary/binary pick uses arity
  (member-ness counts as one operand): `plus` with one member parameter →
  `pl`. `literal <suffix>` → `li<len><suffix>`; `conversion-terminal <type>`
  → `cv<type>`.
- Local names: `Z <ctx-encoding> E <entity> [<discriminator>]`; context
  encodings share the outer substitution table; raw contexts are emitted
  verbatim. Discriminator facts are occurrence numbers: n ≤ 1 emits nothing,
  n ≥ 2 emits `_<n-2>` (Itanium n-2 rule). Lambda numbers are emitted
  verbatim before `_` (`0` → `UlvE0_`); `-` means none.
- Special names: `TI/TV/TT` + type, `TC<derived><off>_<base>`, `TW` + var,
  thunks `Th<nv>_`, covariant `Tc<nv>_<nv>_`, virtual `Tv0_n<off>_`, each
  followed by the target function's full encoding.
- Dependent NTTP arguments (`dependent-value`) use the newer-ABI
  `Tn <type>` prefix before the literal (`TnT_Li3E`).
- Expressions are data-driven prefix manglings: facts carry the Itanium
  operator codes (`dv`, `rm`, `qu` via `conditional`, `cl`, `sc`, `dt`,
  `sp`, `st`, `u`-traits, `fp_`, `T_`, `L<type><val>E`); `member` facts carry
  an explicit close-owner flag for the `sr <type> E <name>` form.
- `c-function` emits the plain name. Multi-case files emit one line per case
  in file order; any parse/resolution failure exits nonzero.

## Ownership boundaries

- `dev/abimangle.cpp`: CLI driver only (starter kit shape) — reads inputs,
  calls `abi_mangle::mangle_fact_files`, writes the output file.
- `abi_fact_parse.cpp` owns text → `AbiFactFile`.
- `abi_fact_serialize.cpp` owns `AbiFactFile` → canonical text.
- `abi_mangle_encode.cpp` owns type/template-arg/expression encoding and the
  substitution table; `abi_mangle_names.cpp` owns qualified-name emission,
  function encodings, target dispatch, and file assembly. The split boundary
  is the internal header's `EncodeContext`.
- No reference binaries, host compilers, or demanglers are invoked by the
  implementation.

## Validation

- `make -C pa30 test` for the suite; `make check TEST=tests/abi/<case>.t`
  during bring-up (100 → 200 → 300 → 400/500/600 order).
- Round-trip `parse → serialize → parse → mangle` must be stable (exercised
  in bring-up, not by the harness).
- Exit criteria: `perl scripts/cppgm_file_audit.pl --stage pa30 --paths
  dev/src` clean and root `make test-report-through-pa30` clean.

## Architecture Review

Reviewed after the implementation landed (`bf6b359a6`), against the README
contract and `doc/itanium-mangling.txt`.

- **Module boundaries hold.** `dev/abimangle.cpp` is the starter-kit CLI
  shape and holds no encoding logic. The parser produces the handout
  scaffold's typed records (`dev/src/abi_mangle.h` is unmodified handout);
  the serializer is its canonical inverse; the encoder is split at the
  `EncodeContext` seam declared in `dev/src/abi/abi_mangle_encode.h`
  (types/args/exprs vs. names/encodings/targets). No module reparses
  another's assembled strings: names are built from `NameLink` structure and
  substitution decisions use canonical keys, not emitted text.
- **Substitution identity is structural and single-owner.** All keys are
  produced by `type_key`/`argument_key`/`expression_key` or carried
  explicitly by the facts; the table itself only maps keys to seq-ids.
  Key unification (named-type keys are qualified names) is what lets parsed
  parameter types hit entries created from `name-source` facts, so no
  second bookkeeping channel exists.
- **Failure paths are loud.** Unknown keywords, heads, operators, terminals,
  and dangling `*_ref`s all throw `AbiFactError`; the driver maps any
  exception to a nonzero exit. There are no fallback name spellings.
- **Fixture-pinned quirks are contained.** The `template-param` vs
  `template-param-subst` split, the member-template terminal-slot rule, and
  the `Tn` dependent-value prefix are recorded in "Encoding rules pinned by
  fixtures" above and implemented in exactly one place each.

Audit findings (defects fixed; details in `pa30/audit.md`):

- unqualified operator terminals counted an implicit object argument, so a
  global unary `operator-` encoded as binary `mi` — member-ness now comes
  from prefix components only;
- plain (non-address-of) entity template arguments were wrapped as
  `XL…EE` instead of the direct `L…E` expr-primary the template-arg grammar
  specifies;
- discriminator numbers ≥ 10 lacked the `__<n>_` spelling;
- the serializer dropped `volatile` from combined `const:volatile:` layers
  and emitted raw array bounds as unparseable `array::…`;
- parsing accepted negative construction-vtable offsets (wrapping through
  an unsigned cast), non-numeric discriminators (silently meaning "none"),
  silent 19-digit integer overflow, and duplicate `let-*` ids (last-wins).

## Final Architecture Review

After the audit fixes, the implementation matches the plan as written: three
concerns in three modules plus a names/encodings unit, one substitution
table per mangled name keyed structurally, facts validated loudly at parse
time, and encoding rules that follow `doc/itanium-mangling.txt` where the
fixtures do not pin behavior (expr-primary entity arguments, two-form
discriminators, non-member operator arity). Round-trip
`parse → serialize → parse → serialize/mangle` is stable over all 73
fixtures plus synthetic coverage of the repaired spellings; the pa30 suite,
the pa30 file audit, and `make test-report-through-pa30` all pass. The
encoder remains a standalone `dev/src/abi/` library with no fact-file or
CLI dependencies in the encoding path, ready for PA31+ compiler-side reuse.

## Stage handoff

PA31+ starts consuming host-object EH facts; the encoder here stays a
standalone library under `dev/src/abi/` so later stages can link the same
Itanium encoder for compiler-side name generation without the fact-file
front end.
