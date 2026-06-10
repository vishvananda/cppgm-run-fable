#pragma once

#include <string>

using std::string;

#include "sema/expr.h"
#include "sema/program.h"

// PA8 initialization semantics (8.5). All entry points throw
// runtime_error on diagnosable ill-formed initializations; a valid
// but non-constant initializer records zero-initialization (the mock
// image has no dynamic initialization code).

// Applies `= init` to the object in `slot` (a variable, or a
// temporary through reference-binding recursion): scalar
// copy-initialization via the standard conversions (8.5p17),
// character arrays from string literals with bound completion
// (8.5.2 - may replace slot.type), and reference binding (8.5.3),
// which may create lifetime-extended temporaries in `program`.
// When slot.is_constexpr the initializer must be constant (7.1.5p9).
void ApplyInitializer(Program& program, ImageSlot& slot, const Expr& init,
                      const string& name);

// Definition without an initializer: zero-initialization. Rejects
// references (8.5.3p3), objects of const-qualified type (8.5p11),
// constexpr objects (7.1.5p9), and incomplete object types.
void ApplyDefaultInitialization(ImageSlot& slot, const string& name);

// 7p4: the static_assert condition, a constant expression
// contextually converted to bool.
bool EvaluateStaticAssertCondition(const Expr& expr, int current_tu);

// 8.3.4p1: an array bound is a converted constant expression of type
// std::size_t (integral, non-narrowing) with a value greater than
// zero.
unsigned long long EvaluateArrayBound(const Expr& expr, int current_tu);
