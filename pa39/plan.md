# PA39 Inception Plan

PA39 adds no new compiler surface. The work is: make the existing
`../dev/cppgm++` rebuild every checkpoint tool from `frontend_source_sets.mk`
(`*-self`), keep the PA1-PA38 preservation ladder green under the self-built
checkpoints, then prove reproducibility (`*-self` rebuilds itself into a
byte-identical `*-inception`). Every ladder failure is treated as an earlier
compiler bug or a reproducibility bug until proven otherwise.

## Current checkpoint: pptoken-self (PA1 rung)

`make -C pa39 test-through-pa10 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++` fails
in the very first rung: host-seeded `../dev/cppgm++` cannot compile four of
the pptoken checkpoint sources.

### Failure 1: "unterminated comment" (source_translation, pp_tokenizer, pptoken)

Comments in `dev/src/source_translation.h` (included by all three failing
TUs), `dev/src/source_translation.cpp`, and `dev/src/ctrl_expr.cpp` spell the
raw character sequence `\UFFFFFFFF` while documenting the end-of-input marker.
The course translation pipeline decodes universal-character-names during
phases 1-2, before comments are stripped, and `\UFFFFFFFF` wraps to the
end-of-input marker, cutting the file off inside a `//` comment.

This is **not** a compiler bug: `pptoken-ref` rejects the same file at the
same spot (`ERROR:9:73:Unterminated comment` on source_translation.h), so the
source text is invalid under the course dialect for any conforming course
compiler, host-seeded or self-built. Per the working rules, a source rewrite
is the correct fix exactly because the source is wrong under host-seeded
`../dev/cppgm++` (and the reference) too. Fix: reword the three comments so
they no longer contain a raw UCN spelling; behavior of the compiled code is
unchanged. A repo sweep found no other raw `\u`/`\U` + hex sequences in
`dev/`.

### Failure 2: "opaque unscoped enum declaration" (test_runner)

`dev/src/test_runner.cpp` includes `<fcntl.h>`; glibc's `struct f_owner_ex`
declares the member `enum __pid_type type;`. `DeclBinder::BindEnum`
(`dev/src/sema/decl_enum.cpp`, PA11 7.2 surface) treats every bodyless,
baseless, unscoped `DK_ENUM` as an opaque-enum-declaration and throws — but
an elaborated-type-specifier `enum E` used with a declarator merely refers to
a previously declared enumeration (7.1.6.3p3 / 3.4.4p2). `cppgm++-ref`
accepts `enum E e;` members, `enum E x = A;` declarations, and `enum E f();`
declarators, and compiles the `<fcntl.h>` probe; it still rejects standalone
`enum E;` (with or without a prior declaration) and elaborated references to
undeclared enums.

Real compiler bug, earliest owning surface PA11 (the enum entity model in
`sema/decl_enum.cpp`; the parse side already accepts the form). The PA7/PA8
`nsdecl` model has no enum support at all and none of its fixtures exercise
enums, so PA11 is the earliest surface that owns this behavior. Fix: give
`BindEnum` an `elaborated` mode (mirroring `BindClassForward`) that resolves
the name by unqualified lookup and requires an enumeration; wire it from
`BindNestedTypeSpecifier`. Reducer: new course tests under
`cppgm.tests/course/pa11/` with fixtures regenerated via
`make -C pa11 ref-test TEST=course/pa11/<test>.t`.

## Validation plan

1. `make -C pa39 probe-self-object SOURCE=...` on each previously failing TU.
2. Reducer tests pass; `make test-report-through-pa38` stays green.
3. `make -C pa39 test-through-pa10 CXX=../dev/cppgm++ CPPGM_HOST_CXX=g++`
   (the blocker) — iterate on the next first failure this exposes, same
   method: reduce, find owning surface, fix there, add reducer.
4. `make -C pa39 compare-pptoken-inception ...` as the first reproducibility
   compare, then the full `compare-cppgm++-inception` target.

## Ladder expectations

Self-built checkpoints must behave identically to host-seeded builds on the
same inputs; >5x slowdowns, timeouts, or OOM in `*-self`/`*-inception`
compiles are layer divergence to trace back to a miscompiled self compiler,
not something to tune around. No self-hosting special cases, no generated
source discovery, no harness weakening.
