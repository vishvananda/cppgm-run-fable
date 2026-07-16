#include "sema/type.h"

#include <stdexcept>

#include "sema/scope.h"
#include "sema/template_info.h"

using std::make_shared;
using std::runtime_error;
using std::to_string;

bool IsIntegralFundamental(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_INT:
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_CHAR:
	case FT_UNSIGNED_SHORT_INT:
	case FT_UNSIGNED_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
	case FT_WCHAR_T:
	case FT_CHAR:
	case FT_CHAR16_T:
	case FT_CHAR32_T:
	case FT_BOOL:
	case FT_INT128:
	case FT_UINT128:
		return true;
	default:
		return false;
	}
}

bool IsSignedIntegralFundamental(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_INT:
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_INT128:
	case FT_CHAR:     // signed on the x86-64 ABI
	case FT_WCHAR_T:  // int on the x86-64 ABI
		return true;
	default:
		return false;
	}
}

bool IsFloatingFundamental(EFundamentalType type)
{
	return type == FT_FLOAT || type == FT_DOUBLE || type == FT_LONG_DOUBLE;
}

bool IsArithmeticType(const TypePtr& type)
{
	return type->kind == TK_FUNDAMENTAL &&
		(IsIntegralFundamental(type->fundamental) ||
		 IsFloatingFundamental(type->fundamental));
}

bool IsIntegralType(const TypePtr& type)
{
	return type->kind == TK_FUNDAMENTAL &&
		IsIntegralFundamental(type->fundamental);
}

bool IsReferenceType(const TypePtr& type)
{
	return type->kind == TK_LVALUE_REFERENCE ||
		type->kind == TK_RVALUE_REFERENCE;
}

bool IsNullPtrType(const TypePtr& type)
{
	return type->kind == TK_FUNDAMENTAL &&
		type->fundamental == FT_NULLPTR_T;
}

bool IsVoidType(const TypePtr& type)
{
	return type->kind == TK_FUNDAMENTAL && type->fundamental == FT_VOID;
}

bool TypeContainsAutoPlaceholder(const TypePtr& type)
{
	for (TypePtr at = type; at; at = at->target)
		if (at->is_auto_placeholder)
			return true;
	return false;
}

TypePtr MakeFundamentalType(EFundamentalType fundamental)
{
	Type type;
	type.kind = TK_FUNDAMENTAL;
	type.fundamental = fundamental;
	return make_shared<Type>(type);
}

TypePtr MakeNamedType(ETypeKind kind, const NamedTypeInfo* info)
{
	if (kind != TK_CLASS && kind != TK_ENUM && kind != TK_TYPE_PARAM)
		throw runtime_error("not a named type kind");
	Type type;
	type.kind = kind;
	type.named = info;
	return make_shared<Type>(type);
}

TypePtr MakePointerType(const TypePtr& pointee, bool is_const,
                        bool is_volatile)
{
	if (IsReferenceType(pointee))
		throw runtime_error("pointer to reference is ill-formed");
	Type type;
	type.kind = TK_POINTER;
	type.is_const = is_const;
	type.is_volatile = is_volatile;
	type.target = pointee;
	return make_shared<Type>(type);
}

TypePtr MakeReferenceType(const TypePtr& target, bool is_rvalue,
                          bool allow_collapse)
{
	// The PA24 auto placeholder composes under reference declarators
	// (`auto&`, `auto&&`); deduction replaces it before use.
	if (IsVoidType(target) && !target->is_auto_placeholder)
		throw runtime_error("reference to void is ill-formed");
	Type type;
	type.target = target;
	if (IsReferenceType(target))
	{
		if (!allow_collapse)
			throw runtime_error("reference to reference is ill-formed");
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
	if (IsReferenceType(element) || element->kind == TK_FUNCTION ||
	    IsVoidType(element) ||
	    (element->kind == TK_ARRAY && !element->bound_known))
		throw runtime_error("invalid array element type");
	Type type;
	type.kind = TK_ARRAY;
	type.target = element;
	type.bound_known = bound_known;
	type.bound = bound;
	return make_shared<Type>(type);
}

TypePtr MakeRefQualifiedType(const TypePtr& function, int ref_qual)
{
	Type qualified = *function;
	qualified.ref_qual = ref_qual;
	return TypePtr(new Type(qualified));
}

TypePtr MakeFunctionType(const TypePtr& return_type,
                         const vector<TypePtr>& parameters, bool variadic)
{
	if (return_type->kind == TK_FUNCTION || return_type->kind == TK_ARRAY)
		throw runtime_error("function returning function or array");
	Type type;
	type.kind = TK_FUNCTION;
	type.target = return_type;
	type.parameters = parameters;
	type.variadic = variadic;
	return make_shared<Type>(type);
}

TypePtr MakeMemberPointerType(const NamedTypeInfo* cls,
                              const TypePtr& member, bool is_const,
                              bool is_volatile)
{
	if (IsReferenceType(member) || IsVoidType(member))
		throw runtime_error("invalid member-pointer member type");
	Type type;
	type.kind = TK_MEMBER_POINTER;
	type.is_const = is_const;
	type.is_volatile = is_volatile;
	type.named = cls;
	type.target = member;
	return make_shared<Type>(type);
}

TypePtr MakeCvQualifiedType(const TypePtr& type, bool add_const,
                            bool add_volatile)
{
	if (!add_const && !add_volatile)
		return type;
	if (IsReferenceType(type))
		return type;
	if (type->kind == TK_FUNCTION)
		// 8.3.5p6: cv-qualifiers reaching a function type through a
		// typedef or template type argument are ignored (a member
		// function's own cv-suffix applies via
		// MakeFunctionCvQualifiedType).
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

// The cv-qualifier-seq of a member function declarator (8.3.5p6):
// unlike typedef- or template-carried cv, it qualifies the function
// type itself.
TypePtr MakeFunctionCvQualifiedType(const TypePtr& type, bool add_const,
                                    bool add_volatile)
{
	if (!add_const && !add_volatile)
		return type;
	Type qualified = *type;
	qualified.is_const = qualified.is_const || add_const;
	qualified.is_volatile = qualified.is_volatile || add_volatile;
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
	if (TypeEquals(existing, redeclared))
		return existing;
	if (existing->kind == TK_ARRAY && redeclared->kind == TK_ARRAY &&
	    TypeEquals(existing->target, redeclared->target))
	{
		if (!existing->bound_known && redeclared->bound_known)
			return redeclared;
		if (existing->bound_known && !redeclared->bound_known)
			return existing;
	}
	throw runtime_error("redeclared with a different type");
}

bool TypeEquals(const TypePtr& a, const TypePtr& b)
{
	if (a.get() == b.get())
		return true;
	if (a->kind != b->kind || a->is_const != b->is_const ||
	    a->is_volatile != b->is_volatile ||
	    a->pack_expansion != b->pack_expansion ||
	    a->bound_param != b->bound_param)
		return false;
	switch (a->kind)
	{
	case TK_FUNDAMENTAL:
		return a->fundamental == b->fundamental;
	case TK_CLASS:
	case TK_ENUM:
	case TK_TYPE_PARAM:
		return a->named == b->named;
	case TK_ARRAY:
		if (a->bound_known != b->bound_known ||
		    (a->bound_known && a->bound != b->bound))
			return false;
		break;
	case TK_FUNCTION:
		if (a->variadic != b->variadic || a->ref_qual != b->ref_qual ||
		    a->parameters.size() != b->parameters.size())
			return false;
		for (size_t i = 0; i < a->parameters.size(); i++)
			if (!TypeEquals(a->parameters[i], b->parameters[i]))
				return false;
		break;
	case TK_MEMBER_POINTER:
		if (a->named != b->named)
			return false;
		break;
	case TK_TEMPLATE_SPEC:
	{
		if (a->named != b->named || a->targs.size() != b->targs.size())
			return false;
		for (size_t i = 0; i < a->targs.size(); i++)
			if (!TemplateArgEquals(a->targs[i], b->targs[i]))
				return false;
		return true;
	}
	default:
		break;
	}
	return TypeEquals(a->target, b->target);
}

TypePtr MakeTemplateSpecType(const NamedTypeInfo* anchor,
                             const vector<TemplateArg>& args)
{
	Type type;
	type.kind = TK_TEMPLATE_SPEC;
	type.named = anchor;
	type.targs = args;
	return make_shared<Type>(type);
}

bool TemplateArgEquals(const TemplateArg& a, const TemplateArg& b)
{
	if (a.is_value != b.is_value || a.pack_pattern != b.pack_pattern ||
	    a.dependent_type != b.dependent_type)
		return false;
	// PA21 template-template arguments compare by template identity.
	if (a.template_entity || b.template_entity ||
	    a.template_param >= 0 || b.template_param >= 0)
		return a.template_entity == b.template_entity &&
			a.template_param == b.template_param;
	if (!a.is_value)
		return TypeEquals(a.type, b.type);
	if (a.value_param != b.value_param ||
	    bool(a.dependent_value) != bool(b.dependent_value))
		return false;
	if (a.dependent_value)
		return a.dependent_value == b.dependent_value;
	// PA22 entity-valued arguments compare by entity identity.
	if (a.entity_scope || b.entity_scope)
		return a.entity_scope == b.entity_scope &&
			a.entity_name == b.entity_name;
	return TypeEquals(a.type, b.type) && a.value_bits == b.value_bits;
}

bool TemplateArgIsDependent(const TemplateArg& arg)
{
	if (arg.value_param >= 0 || arg.dependent_value || arg.pack_pattern)
		return true;
	if (arg.dependent_type)
		return true;
	if (arg.template_param >= 0)
		return true;
	if (arg.template_entity)
		return false;
	return TypeIsDependent(arg.type);
}

bool TypeIsDependent(const TypePtr& type)
{
	if (!type)
		return false;
	if (type->kind == TK_TYPE_PARAM || type->kind == TK_TEMPLATE_SPEC)
		return true;
	if (type->bound_param >= 0)
		return true;
	// PA26: a member pointer whose class part names a parameter.
	if (type->kind == TK_MEMBER_POINTER && type->named &&
	    type->named->param_index >= 0)
		return true;
	for (size_t i = 0; i < type->parameters.size(); i++)
		if (TypeIsDependent(type->parameters[i]))
			return true;
	return TypeIsDependent(type->target);
}

TypePtr RemoveTopCv(const TypePtr& type)
{
	if (type->kind == TK_ARRAY)
	{
		TypePtr element = RemoveTopCv(type->target);
		if (element.get() == type->target.get())
			return type;
		Type stripped = *type;
		stripped.target = element;
		return make_shared<Type>(stripped);
	}
	if (!type->is_const && !type->is_volatile)
		return type;
	Type stripped = *type;
	stripped.is_const = false;
	stripped.is_volatile = false;
	return make_shared<Type>(stripped);
}

void TopCv(const TypePtr& type, bool& is_const, bool& is_volatile)
{
	if (type->kind == TK_ARRAY)
	{
		TopCv(type->target, is_const, is_volatile);
		return;
	}
	is_const = type->is_const;
	is_volatile = type->is_volatile;
}

// 4.4p4: T1* -> T2* when both are "similar" (same structure of pointer
// levels onto the same base type), cv at every level only grows, and
// any level that grows is preceded by const at every higher level.
bool QualificationConvertible(const TypePtr& from, const TypePtr& to)
{
	if (from->kind != TK_POINTER || to->kind != TK_POINTER)
		return false;
	bool const_above = true;  // const at all of levels 1..k-1 so far
	const Type* f = from.get();
	const Type* t = to.get();
	while (f->kind == TK_POINTER && t->kind == TK_POINTER)
	{
		const Type* fp = f->target.get();
		const Type* tp = t->target.get();
		if (fp->is_const && !tp->is_const)
			return false;
		if (fp->is_volatile && !tp->is_volatile)
			return false;
		if ((fp->is_const != tp->is_const ||
		     fp->is_volatile != tp->is_volatile) &&
		    !const_above)
			return false;
		const_above = const_above && tp->is_const;
		f = fp;
		t = tp;
	}
	// The deepest level's cv growth was validated by the loop; the base
	// types must otherwise be identical.
	if (f->kind != t->kind)
		return false;
	Type fs = *f;
	Type ts = *t;
	fs.is_const = ts.is_const = false;
	fs.is_volatile = ts.is_volatile = false;
	return TypeEquals(make_shared<Type>(fs), make_shared<Type>(ts));
}

// 13.3.3.2p3: whether similar type `a`'s cv-qualification signature is
// a proper subset of `b`'s (cv never exceeds at any level, and at
// least one level is strictly smaller).
bool CvSignatureProperSubset(const TypePtr& a, const TypePtr& b)
{
	const Type* x = a.get();
	const Type* y = b.get();
	bool proper = false;
	while (x->kind == TK_POINTER && y->kind == TK_POINTER)
	{
		const Type* xp = x->target.get();
		const Type* yp = y->target.get();
		if ((xp->is_const && !yp->is_const) ||
		    (xp->is_volatile && !yp->is_volatile))
			return false;
		if (xp->is_const != yp->is_const ||
		    xp->is_volatile != yp->is_volatile)
			proper = true;
		x = xp;
		y = yp;
	}
	if (x->kind == TK_POINTER || y->kind == TK_POINTER)
		return false;
	return proper;
}

static unsigned long long FundamentalSize(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
	case FT_CHAR:
	case FT_BOOL:
		return 1;
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T:
		return 2;
	case FT_INT:
	case FT_UNSIGNED_INT:
	case FT_WCHAR_T:
	case FT_CHAR32_T:
	case FT_FLOAT:
		return 4;
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
	case FT_DOUBLE:
	case FT_NULLPTR_T:
		return 8;
	case FT_LONG_DOUBLE:
	case FT_INT128:
	case FT_UINT128:
		return 16;
	case FT_VOID:
		break;
	}
	throw runtime_error("void is an incomplete type");
}

// TK_CLASS / TK_ENUM / TK_TYPE_PARAM: layout facts live on the shared
// entity record once the entity completes. A zero alignment marks an
// entity whose layout could not be computed (dependent member types in
// a template), which has no size for PA11 purposes.
static const NamedTypeInfo& CompleteNamedInfo(const TypePtr& type)
{
	if (type->kind == TK_TYPE_PARAM || type->kind == TK_TEMPLATE_SPEC ||
	    !type->named->complete ||
	    type->named->alignment == 0)
		throw runtime_error(type->named->display + " is an incomplete type");
	return *type->named;
}

unsigned long long TypeSize(const TypePtr& type)
{
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		return FundamentalSize(type->fundamental);
	case TK_POINTER:
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
		return 8;
	case TK_MEMBER_POINTER:
		// Itanium ABI: {ptr} for data members, {ptr, adj} for functions.
		return type->target->kind == TK_FUNCTION ? 16 : 8;
	case TK_CLASS:
	case TK_ENUM:
	case TK_TYPE_PARAM:
	case TK_TEMPLATE_SPEC:
		return CompleteNamedInfo(type).size;
	case TK_ARRAY:
	{
		if (!type->bound_known)
			throw runtime_error("array of unknown bound is incomplete");
		unsigned long long element = TypeSize(type->target);
		if (element != 0 && type->bound > ~0ull / element)
			throw runtime_error("array size exceeds implementation limits");
		return type->bound * element;
	}
	case TK_FUNCTION:
		break;
	}
	throw runtime_error("function types have no size");
}

unsigned long long TypeAlignment(const TypePtr& type)
{
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		return FundamentalSize(type->fundamental);
	case TK_POINTER:
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	case TK_MEMBER_POINTER:
		return 8;
	case TK_CLASS:
	case TK_ENUM:
	case TK_TYPE_PARAM:
	case TK_TEMPLATE_SPEC:
		return CompleteNamedInfo(type).alignment;
	case TK_ARRAY:
		return TypeAlignment(type->target);
	case TK_FUNCTION:
		break;
	}
	throw runtime_error("function types have no alignment");
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
	case TK_CLASS:
	case TK_ENUM:
	case TK_TYPE_PARAM:
		return cv + type->named->display;
	case TK_POINTER:
		return cv + "pointer to " + DescribeType(type->target);
	case TK_MEMBER_POINTER:
		return cv + "member-pointer of " + type->named->display + " to " +
			DescribeType(type->target);
	case TK_TEMPLATE_SPEC:
	{
		string args;
		for (size_t i = 0; i < type->targs.size(); i++)
		{
			if (i > 0)
				args += ", ";
			if (!type->targs[i].is_value)
				args += DescribeType(type->targs[i].type);
			else if (type->targs[i].value_param >= 0)
				args += "value-parameter #" +
					to_string(type->targs[i].value_param);
			else if (type->targs[i].dependent_value)
				args += "dependent-value";
			else
				args += to_string(
					(long long)type->targs[i].value_bits);
		}
		return cv + type->named->display + "<" + args + ">";
	}
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
	// Member-function cv-qualifiers print between the parameter list and
	// the return type (`function of () const returning int`).
	string qualifiers;
	if (type->is_const)
		qualifiers += " const";
	if (type->is_volatile)
		qualifiers += " volatile";
	if (type->ref_qual == 1)
		qualifiers += " &";
	else if (type->ref_qual == 2)
		qualifiers += " &&";
	return "function of (" + parameters + ")" + qualifiers + " returning " +
		DescribeType(type->target);
}

bool AnySimpleTypeSpecifier(const SimpleTypeSpecifiers& specs)
{
	return specs.has_base || specs.signed_count > 0 ||
		specs.unsigned_count > 0 || specs.short_count > 0 ||
		specs.long_count > 0;
}

EFundamentalType CombineSimpleTypeSpecifiers(
	const SimpleTypeSpecifiers& specs)
{
	if (!AnySimpleTypeSpecifier(specs))
		throw runtime_error("declaration requires a type specifier");
	bool is_unsigned = specs.unsigned_count > 0;
	if (specs.signed_count + specs.unsigned_count > 1 ||
	    specs.short_count > 1 || specs.long_count > 2 ||
	    (specs.short_count && specs.long_count))
		throw runtime_error("invalid type specifier combination");
	bool modified = specs.signed_count || specs.unsigned_count ||
		specs.short_count || specs.long_count;
	switch (specs.has_base ? specs.base : KW_INT)
	{
	case KW_CHAR:
		if (specs.short_count || specs.long_count)
			throw runtime_error("invalid type specifier combination");
		if (is_unsigned)
			return FT_UNSIGNED_CHAR;
		return specs.signed_count ? FT_SIGNED_CHAR : FT_CHAR;
	case KW_CHAR16_T:
	case KW_CHAR32_T:
	case KW_WCHAR_T:
	case KW_BOOL:
	case KW_FLOAT:
	case KW_VOID:
		if (modified)
			throw runtime_error("invalid type specifier combination");
		switch (specs.base)
		{
		case KW_CHAR16_T: return FT_CHAR16_T;
		case KW_CHAR32_T: return FT_CHAR32_T;
		case KW_WCHAR_T: return FT_WCHAR_T;
		case KW_BOOL: return FT_BOOL;
		case KW_FLOAT: return FT_FLOAT;
		default: return FT_VOID;
		}
	case KW_DOUBLE:
		if (specs.signed_count || specs.unsigned_count ||
		    specs.short_count || specs.long_count > 1)
			throw runtime_error("invalid type specifier combination");
		return specs.long_count ? FT_LONG_DOUBLE : FT_DOUBLE;
	default:  // KW_INT, spelled or implied
		if (specs.short_count)
			return is_unsigned ? FT_UNSIGNED_SHORT_INT : FT_SHORT_INT;
		if (specs.long_count == 2)
			return is_unsigned ? FT_UNSIGNED_LONG_LONG_INT
			                   : FT_LONG_LONG_INT;
		if (specs.long_count == 1)
			return is_unsigned ? FT_UNSIGNED_LONG_INT : FT_LONG_INT;
		return is_unsigned ? FT_UNSIGNED_INT : FT_INT;
	}
}

// BaseClassDistance lives in class_info.cpp: with multiple direct bases
// (PA26) the derivation relation is a DAG walk over the class records,
// not a chase up the base_entity chain.

// PA25 18.9: the class template named initializer_list declared
// directly inside the top-level namespace std.
bool IsStdInitializerListTemplate(const TemplateInfo* tmpl)
{
	return tmpl && tmpl->name == "initializer_list" &&
		tmpl->declaring && tmpl->declaring->kind == SCOPE_NAMESPACE &&
		tmpl->declaring->name == "std" && tmpl->declaring->parent &&
		!tmpl->declaring->parent->parent;
}

// PA25 18.9: a std::initializer_list specialization.
bool IsStdInitializerList(const TypePtr& type, TypePtr* element)
{
	if (!type)
		return false;
	TypePtr bare = RemoveTopCv(
		IsReferenceType(type) ? type->target : type);
	if ((bare->kind != TK_CLASS && bare->kind != TK_TEMPLATE_SPEC) ||
	    !bare->named ||
	    !IsStdInitializerListTemplate(bare->named->spec_template))
		return false;
	if (bare->named->spec_args.empty() ||
	    !bare->named->spec_args[0].type)
		return false;
	if (element)
		*element = bare->named->spec_args[0].type;
	return true;
}
