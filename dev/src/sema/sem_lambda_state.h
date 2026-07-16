#pragma once

#include <string>
#include <vector>

#include "sema/type.h"

// PA24 lambda-binding state (sem_lambda.cpp): the capture records,
// the open-body frame, and the cached synthesized identity of one
// analyzed lambda-expression. SemBinder owns instances of these;
// they carry no behavior of their own.

struct Scope;
struct ScopeBinding;
struct ClassInfo;

// One capture of an open (or synthesized) lambda, in field order.
struct SemLambdaCapture
{
	const ScopeBinding* binding = 0;  // null for `this`
	std::string name;
	unsigned long long offset = 0;
	TypePtr referee;  // captured entity type (references stripped)
	bool is_this = false;
	bool by_copy = false;  // PA25: a value field, not a reference
};

// The body-binding context of one lambda whose statements are
// currently being bound; `cls` is null for a captureless lambda.
struct SemLambdaFrame
{
	Scope* fn_scope = 0;
	ClassInfo* cls = 0;
	Scope* members = 0;
	bool by_ref_default = false;   // [&] spelled
	bool by_copy_default = false;  // PA25: [=] spelled
	bool this_spelled = false;     // [this] spelled
	bool is_mutable = false;       // `mutable` spelled
	TypePtr this_param_type;       // the closure's own this
	TypePtr enclosing_this;        // the captured-this type (or null)
	std::vector<std::string> explicit_names;  // [&name] / [name]
	std::vector<char> explicit_copy;          // parallel: by copy
	std::vector<SemLambdaCapture> captures;
	bool this_captured = false;
	unsigned long long this_offset = 0;
};

// The synthesized identity of an analyzed lambda-expression, cached
// per (lambda AST, enclosing body) so the deduction and
// initialization analyses of one declaration share one synthesis.
struct SemLambdaInfo
{
	bool captureless = true;
	std::string fn_name;  // captureless: the internal function
	TypePtr fn_type;
	ClassInfo* cls = 0;   // capturing: the closure class
	std::vector<SemLambdaCapture> captures;
};
