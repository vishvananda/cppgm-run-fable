#include "sema/type.h"

using std::make_shared;
using std::to_string;

TypePtr MakeFundamentalType(EFundamentalType fundamental)
{
	Type type;
	type.kind = TK_FUNDAMENTAL;
	type.fundamental = fundamental;
	return make_shared<Type>(type);
}

TypePtr MakePointerType(const TypePtr& pointee, bool is_const,
                        bool is_volatile)
{
	Type type;
	type.kind = TK_POINTER;
	type.is_const = is_const;
	type.is_volatile = is_volatile;
	type.target = pointee;
	return make_shared<Type>(type);
}

TypePtr MakeReferenceType(const TypePtr& target, bool is_rvalue)
{
	Type type;
	type.target = target;
	if (target->kind == TK_LVALUE_REFERENCE ||
	    target->kind == TK_RVALUE_REFERENCE)
	{
		type.kind = (is_rvalue && target->kind == TK_RVALUE_REFERENCE)
			? TK_RVALUE_REFERENCE : TK_LVALUE_REFERENCE;
		type.target = target->target;
	}
	else
		type.kind = is_rvalue ? TK_RVALUE_REFERENCE : TK_LVALUE_REFERENCE;
	return make_shared<Type>(type);
}

TypePtr MakeArrayType(const TypePtr& element, bool bound_known,
                      unsigned long long bound)
{
	Type type;
	type.kind = TK_ARRAY;
	type.target = element;
	type.bound_known = bound_known;
	type.bound = bound;
	return make_shared<Type>(type);
}

TypePtr MakeFunctionType(const TypePtr& return_type,
                         const vector<TypePtr>& parameters, bool variadic)
{
	Type type;
	type.kind = TK_FUNCTION;
	type.target = return_type;
	type.parameters = parameters;
	type.variadic = variadic;
	return make_shared<Type>(type);
}

TypePtr MakeCvQualifiedType(const TypePtr& type, bool add_const,
                            bool add_volatile)
{
	if (!add_const && !add_volatile)
		return type;
	if (type->kind == TK_LVALUE_REFERENCE ||
	    type->kind == TK_RVALUE_REFERENCE)
		return type;
	Type qualified = *type;
	if (type->kind == TK_ARRAY)
		qualified.target = MakeCvQualifiedType(type->target, add_const,
		                                       add_volatile);
	else
	{
		qualified.is_const = qualified.is_const || add_const;
		qualified.is_volatile = qualified.is_volatile || add_volatile;
	}
	return make_shared<Type>(qualified);
}

TypePtr AdjustParameterType(const TypePtr& type)
{
	if (type->kind == TK_ARRAY)
		return MakePointerType(type->target, false, false);
	if (type->kind == TK_FUNCTION)
		return MakePointerType(type, false, false);
	if (!type->is_const && !type->is_volatile)
		return type;
	Type stripped = *type;
	stripped.is_const = false;
	stripped.is_volatile = false;
	return make_shared<Type>(stripped);
}

TypePtr MergeRedeclaredType(const TypePtr& existing,
                            const TypePtr& redeclared)
{
	if (existing->kind == TK_ARRAY && !existing->bound_known &&
	    redeclared->kind == TK_ARRAY && redeclared->bound_known)
		return redeclared;
	return existing;
}

string DescribeType(const TypePtr& type)
{
	string cv;
	if (type->is_const)
		cv += "const ";
	if (type->is_volatile)
		cv += "volatile ";
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		return cv + FundamentalTypeName(type->fundamental);
	case TK_POINTER:
		return cv + "pointer to " + DescribeType(type->target);
	case TK_LVALUE_REFERENCE:
		return "lvalue-reference to " + DescribeType(type->target);
	case TK_RVALUE_REFERENCE:
		return "rvalue-reference to " + DescribeType(type->target);
	case TK_ARRAY:
		if (!type->bound_known)
			return "array of unknown bound of " + DescribeType(type->target);
		return "array of " + to_string(type->bound) + " " +
			DescribeType(type->target);
	case TK_FUNCTION:
		break;
	}
	string parameters;
	for (size_t i = 0; i < type->parameters.size(); i++)
	{
		if (i > 0)
			parameters += ", ";
		parameters += DescribeType(type->parameters[i]);
	}
	if (type->variadic)
		parameters += type->parameters.empty() ? "..." : ", ...";
	return "function of (" + parameters + ") returning " +
		DescribeType(type->target);
}
