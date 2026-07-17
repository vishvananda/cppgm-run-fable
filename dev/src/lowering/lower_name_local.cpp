#include "lowering/lower_name.h"

#include <cstdio>
#include <stdexcept>

#include "lowering/lower_name_parts.h"
#include "sema/class_info.h"
#include "sema/scope_lookup.h"

using std::string;
using std::to_string;
using std::vector;

// 5.1.7 local-entity manglings: the enclosing-function encodings of
// function-local classes and closures (recursively for a lambda inside
// a lambda), the Itanium <lambda-sig>, and the Z..E local-name form.
// Split from lower_name.cpp (the general type/name machinery stays
// there).

namespace lower_mangle {

// 5.1.7: the enclosing function scope of a local entity (null when
// the entity is not function-local).
const Scope* LocalEntityFunctionScope(const NamedTypeInfo& info)
{
	for (const Scope* scope = info.scope; scope && scope->parent;
	     scope = scope->parent)
	{
		if (scope->kind == SCOPE_FUNCTION)
			return scope;
		if (scope->kind == SCOPE_NAMESPACE)
			return 0;
	}
	return 0;
}

// 5.1.7: the encoding of the function enclosing a local entity,
// sharing the caller's substitution table so the components register
// for later compression (`ZNS_8lookup_aE...EUlS6_PKcE_`).
string ClosureLambdaSignature(const NamedTypeInfo& info,
                              Substitutions& subs);

string MangleEnclosingFunctionEncoding(const Scope* fn_scope,
                                       Substitutions& subs)
{
	subs.encoding_instance++;
	const Scope* declaring = fn_scope->parent;
	// The body scope carries its own composed type; a name lookup
	// would find only the first overload of the name.
	TypePtr fn_type = fn_scope->fn_type;
	if (!fn_type && declaring)
		if (const ScopeBinding* fn_binding =
		        FindOwnBinding(*declaring, fn_scope->name))
			fn_type = fn_binding->type;
	if (!fn_type || !declaring)
		throw OutsideBoundary("local entity mangling context");
	// main is unmangled, so its local-name prefix is the bare source
	// name with no signature (the g++/reference spelling).
	if (fn_scope->name == "main" &&
	    declaring->kind == SCOPE_NAMESPACE && !declaring->parent)
		return "4main";
	if (fn_scope->fn_spec)
		return MangleFunctionTemplateEncoding(*fn_scope->fn_spec,
		                                      subs);
	// The operator() of a local closure is itself a local entity: its
	// encoding nests the enclosing function's (5.1.7 recursively), so
	// a lambda inside a lambda spells the full Z..EZ..E chain.
	if (fn_scope->closure_entity)
	{
		const NamedTypeInfo& closure = *fn_scope->closure_entity;
		const Scope* outer_fn = LocalEntityFunctionScope(closure);
		if (!outer_fn)
			throw OutsideBoundary("local entity mangling context");
		string outer = MangleEnclosingFunctionEncoding(outer_fn, subs);
		string signature = ClosureLambdaSignature(closure, subs);
		subs.Add("ZL:" + PointerKey(&closure));
		string cv = string(fn_type->is_volatile ? "V" : "") +
			(fn_type->is_const ? "K" : "");
		return "Z" + outer + "EN" + cv + signature + "clE" +
			MangleBareParameters(fn_type, subs, 0);
	}
	if (declaring->kind == SCOPE_CLASS)
	{
		// The member encoding takes the this-adjusted type; a body
		// scope's composed type carries only the declared signature.
		TypePtr adjusted = fn_type;
		bool has_this = !fn_type->parameters.empty() &&
			fn_type->parameters[0]->kind == TK_POINTER &&
			RemoveTopCv(fn_type->parameters[0]->target)->kind ==
				TK_CLASS &&
			RemoveTopCv(fn_type->parameters[0]->target)->named ==
				declaring->entity;
		if (!has_this && declaring->entity)
		{
			TypePtr class_type =
				MakeNamedType(TK_CLASS, declaring->entity);
			class_type = MakeCvQualifiedType(
				class_type, fn_type->is_const, fn_type->is_volatile);
			vector<TypePtr> params;
			params.push_back(
				MakePointerType(class_type, false, false));
			for (size_t i = 0; i < fn_type->parameters.size(); i++)
				params.push_back(fn_type->parameters[i]);
			adjusted = MakeFunctionType(fn_type->target, params,
			                            fn_type->variadic);
		}
		return MangleMemberFunctionEncoding(declaring, fn_scope->name,
		                                    adjusted, "", subs);
	}
	return MangleFunctionEncoding(declaring, fn_scope->name, fn_type,
	                              subs);
}

// The Itanium <lambda-sig> of a closure class
// (Ul <bare-params> E <discriminator> _).
string ClosureLambdaSignature(const NamedTypeInfo& info,
                              Substitutions& subs)
{
	string params = "v";
	if (info.class_record && info.class_record->members)
		if (const ScopeBinding* op = FindOwnBinding(
		        *info.class_record->members, "operator ()"))
			if (op->type && op->type->kind == TK_FUNCTION)
				params = MangleBareParameters(op->type, subs, 0);
	return "Ul" + params + "E" +
		(info.closure_discriminator > 0
			 ? std::to_string(info.closure_discriminator - 1)
			 : string()) + "_";
}

// 5.1.7: a function-local entity mangles as
// `Z <function encoding> E <entity name>`.
string MangleLocalName(const NamedTypeInfo& info, const Scope* fn_scope,
                       Substitutions& subs, string* key_out)
{
	string key = "ZL:" + PointerKey(&info);
	if (key_out)
		*key_out = key;
	string found = subs.Find(key);
	if (!found.empty())
		return found;
	string fn_encoding = MangleEnclosingFunctionEncoding(fn_scope, subs);
	// PA25: a closure class spells the Itanium <lambda-sig> local
	// name (Ul <bare-params> E <discriminator> _).
	if (info.is_closure)
	{
		string signature = ClosureLambdaSignature(info, subs);
		subs.Add(key);
		return "Z" + fn_encoding + "E" + signature;
	}
	// The classes between the function and the entity, then the leaf.
	string names;
	for (const Scope* scope = info.scope;
	     scope && scope != fn_scope; scope = scope->parent)
		if (scope->kind == SCOPE_CLASS && !scope->name.empty())
			names = SourceName(scope->name) + names;
	names += SourceName(info.name);
	subs.Add(key);
	return "Z" + fn_encoding + "E" + names;
}


}  // namespace lower_mangle
