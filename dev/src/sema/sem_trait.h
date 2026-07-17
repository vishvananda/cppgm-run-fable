#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sema/type.h"

// PA34 builtin type traits (__is_same and family): the one evaluation
// over resolved operand types, shared by the semantic expression
// analyzer and the PA11 constant-expression evaluator. The parse-side
// name table is hosted_probes.cpp kBuiltinTraits; the two grow
// together so __has_builtin answers stay honest.
//
// `require_complete` is invoked for operands whose trait inspects
// members (20.9.2 completeness preconditions); the caller supplies its
// completion hook (deferred nested classes complete there).
bool EvaluateBuiltinTraitOnTypes(
	const std::string& name,
	const std::vector<TypePtr>& types,
	const std::function<void(const TypePtr&)>& require_complete);
