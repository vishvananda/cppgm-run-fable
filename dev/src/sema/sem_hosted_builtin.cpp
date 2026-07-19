#include "sema/sem_expr.h"

#include <stdexcept>

#include "sema/class_info.h"
#include "sema/scope_lookup.h"

using std::runtime_error;
using std::to_string;

// PA34 magic-typed hosted builtins: callee forms whose parameter and
// result types derive from their arguments (the fixed-signature
// builtin families lazily declare through
// SemBinder::ResolveBuiltinFunction instead). Each analyzer builds an
// ordinary resolved call node - or the equivalent expression - so
// constant evaluation and lowering consume typed semantic state.

namespace {

Scope* GlobalScopeOf(Scope* scope)
{
	while (scope && scope->parent)
		scope = scope->parent;
	return scope;
}

}  // namespace

// The shared call-node shape of a magic builtin: a global-scope
// SN_CALLEE carrying the synthesized function type over the
// converted arguments.
SemValue SemExprAnalyzer::MakeBuiltinCallResult(const string& name,
                                                const TypePtr& fn_type,
                                                vector<SemValue>& args,
                                                bool no_throw)
{
	SemValue value = CallResult(fn_type);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	Scope* global = GlobalScopeOf(host_.CurrentScope());
	callee->name = CanonicalQualifiedName(global, name);
	callee->type = fn_type;
	callee->entity_scope = global;
	callee->entity_name = name;
	callee->c_linkage = true;
	if (no_throw)
	{
		callee->unwind_no = true;
		callee->noexcept_decl = true;
	}
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

bool SemExprAnalyzer::TryAnalyzeMagicBuiltin(const AstExpr& expr,
                                             const string& name,
                                             SemValue& out)
{
	if (name == "__builtin_constant_p")
		out = AnalyzeBuiltinConstantP(expr);
	else if (name == "__builtin_addressof")
		out = AnalyzeBuiltinAddressOf(expr);
	else if (name == "__builtin_clzg" || name == "__builtin_ctzg" ||
	         name == "__builtin_popcountg")
		out = AnalyzeBuiltinBitCountG(expr, name);
	else if (name == "__builtin_add_overflow" ||
	         name == "__builtin_sub_overflow" ||
	         name == "__builtin_mul_overflow")
		out = AnalyzeBuiltinOverflow(expr, name);
	else if (name == "__builtin_operator_new" ||
	         name == "__builtin_operator_delete")
		out = AnalyzeBuiltinAllocation(expr, name);
	else if (name == "__builtin_fpclassify")
		out = AnalyzeBuiltinFpclassify(expr);
	else if (name == "__builtin_invoke")
		out = AnalyzeBuiltinInvoke(expr);
	else if (name == "__builtin_FUNCTION")
		out = MakeFunctionNameLiteral(host_.CurrentFunctionName());
	else
		return false;
	return true;
}

// A synthesized narrow string literal (__func__ and the
// __builtin_FUNCTION operator): an lvalue array of n const char.
SemValue SemExprAnalyzer::MakeFunctionNameLiteral(const string& text)
{
	SemValue value;
	value.type = MakeArrayType(
		MakeCvQualifiedType(MakeFundamentalType(FT_CHAR), true, false),
		true, text.size() + 1);
	value.category = VC_LVALUE;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = "\"" + text + "\"";
	value.node->is_string_literal = true;
	value.node->string_bytes = text + '\0';
	value.node->type = value.type;
	value.node->category = value.category;
	return value;
}

// 20.6.12.1: the address of the object, never an overloaded
// operator& (the point of the builtin).
SemValue SemExprAnalyzer::AnalyzeBuiltinAddressOf(const AstExpr& expr)
{
	if (expr.arguments.size() != 1)
		throw runtime_error("__builtin_addressof takes one argument");
	SemValue operand = Analyze(*expr.arguments[0]);
	if (operand.category != VC_LVALUE)
		throw runtime_error("__builtin_addressof of a non-lvalue");
	SemValue value;
	value.type = MakePointerType(operand.type, false, false);
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_UNARY_EXPRESSION);
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	value.node->has_op = true;
	value.node->op = OP_AMP;
	value.node->op_spelling = "&";
	value.node->children.push_back(std::move(operand.node));
	return value;
}

// __builtin_{clz,ctz,popcount}g: the count runs over the operand's
// own type width; clzg/ctzg take an optional zero answer.
SemValue SemExprAnalyzer::AnalyzeBuiltinBitCountG(const AstExpr& expr,
                                                  const string& name)
{
	size_t max_args = name == "__builtin_popcountg" ? 1 : 2;
	if (expr.arguments.empty() || expr.arguments.size() > max_args)
		throw runtime_error(name + " argument count");
	vector<SemValue> args;
	AnalyzeArgumentList(expr.arguments, args);
	TypePtr bare = RemoveTopCv(args[0].type);
	if (!IsIntegralType(bare))
		throw runtime_error(name + " requires an integer operand");
	vector<TypePtr> params;
	params.push_back(bare);
	if (args.size() > 1)
		params.push_back(MakeFundamentalType(FT_INT));
	TypePtr fn_type = MakeFunctionType(MakeFundamentalType(FT_INT),
	                                   params, false);
	CheckCallArguments(fn_type, args);
	return MakeBuiltinCallResult(name, fn_type, args, true);
}

// __builtin_{add,sub,mul}_overflow(a, b, out): the same-type family;
// operands compute in the result object's type.
SemValue SemExprAnalyzer::AnalyzeBuiltinOverflow(const AstExpr& expr,
                                                 const string& name)
{
	if (expr.arguments.size() != 3)
		throw runtime_error(name + " takes three arguments");
	vector<SemValue> args;
	AnalyzeArgumentList(expr.arguments, args);
	TypePtr out_type = RemoveTopCv(args[2].type);
	if (out_type->kind != TK_POINTER)
		throw runtime_error(name + " requires a result pointer");
	TypePtr result = RemoveTopCv(out_type->target);
	if (!IsIntegralType(result) ||
	    result->fundamental == FT_BOOL)
		throw runtime_error(name + " requires an integer result");
	vector<TypePtr> params;
	params.push_back(result);
	params.push_back(result);
	params.push_back(MakePointerType(result, false, false));
	TypePtr fn_type = MakeFunctionType(MakeFundamentalType(FT_BOOL),
	                                   params, false);
	CheckCallArguments(fn_type, args);
	return MakeBuiltinCallResult(name, fn_type, args, true);
}

// __builtin_operator_new/__builtin_operator_delete: the usual
// allocation signatures; a trailing enumeration argument selects the
// align_val_t overload (the lowering names the host ABI symbol).
SemValue SemExprAnalyzer::AnalyzeBuiltinAllocation(const AstExpr& expr,
                                                   const string& name)
{
	bool is_new = name == "__builtin_operator_new";
	if (expr.arguments.empty() || expr.arguments.size() > 3)
		throw runtime_error(name + " argument count");
	vector<SemValue> args;
	AnalyzeArgumentList(expr.arguments, args);
	TypePtr size_type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	TypePtr void_ptr = MakePointerType(MakeFundamentalType(FT_VOID),
	                                   false, false);
	vector<TypePtr> params;
	params.push_back(is_new ? size_type : void_ptr);
	for (size_t i = 1; i < args.size(); i++)
	{
		TypePtr bare = RemoveTopCv(args[i].type);
		params.push_back(bare->kind == TK_ENUM ? bare : size_type);
	}
	TypePtr fn_type = MakeFunctionType(
		is_new ? void_ptr : MakeFundamentalType(FT_VOID), params, false);
	CheckCallArguments(fn_type, args);
	return MakeBuiltinCallResult(name, fn_type, args, !is_new);
}

// __builtin_fpclassify(nan, inf, normal, subnormal, zero, value):
// type-generic over the real floating types (an integer operand
// classifies as double).
SemValue SemExprAnalyzer::AnalyzeBuiltinFpclassify(const AstExpr& expr)
{
	if (expr.arguments.size() != 6)
		throw runtime_error("__builtin_fpclassify takes six arguments");
	vector<SemValue> args;
	AnalyzeArgumentList(expr.arguments, args);
	TypePtr operand = RemoveTopCv(args[5].type);
	if (IsIntegralType(operand))
		operand = MakeFundamentalType(FT_DOUBLE);
	if (operand->kind != TK_FUNDAMENTAL ||
	    !IsFloatingFundamental(operand->fundamental))
		throw runtime_error("__builtin_fpclassify requires an "
		                    "arithmetic operand");
	vector<TypePtr> params(5, MakeFundamentalType(FT_INT));
	params.push_back(operand);
	TypePtr fn_type = MakeFunctionType(MakeFundamentalType(FT_INT),
	                                   params, false);
	CheckCallArguments(fn_type, args);
	return MakeBuiltinCallResult("__builtin_fpclassify", fn_type, args,
	                             true);
}

// 20.8.2 INVOKE over the analyzed first argument: member function
// pointers bind their object argument, member data pointers read
// through it, anything else calls (function objects resolve their
// operator() / surrogates through the ordinary functor path).
SemValue SemExprAnalyzer::AnalyzeBuiltinInvoke(const AstExpr& expr)
{
	if (expr.arguments.empty())
		throw runtime_error("__builtin_invoke needs a callable");
	SemValue callable = Analyze(*expr.arguments[0]);
	TypePtr bare = RemoveTopCv(callable.type);
	if (bare->kind == TK_MEMBER_POINTER)
	{
		if (expr.arguments.size() < 2)
			throw runtime_error("__builtin_invoke member pointer "
			                    "needs an object");
		SemValue object = Analyze(*expr.arguments[1]);
		bool arrow = RemoveTopCv(object.type)->kind == TK_POINTER;
		AstExpr access_shape(EK_BINARY);
		access_shape.op = arrow ? OP_ARROWSTAR : OP_DOTSTAR;
		access_shape.op_spelling = arrow ? "->*" : ".*";
		SemValue access = AnalyzeMemberPointerBinary(access_shape,
		                                             object, callable);
		if (access.type->kind != TK_FUNCTION)
		{
			if (expr.arguments.size() != 2)
				throw runtime_error("__builtin_invoke data member "
				                    "takes one object");
			return access;
		}
		vector<SemValue> args;
		AnalyzeArgumentList(expr.arguments, args, false, 2);
		CheckCallArguments(access.type, args);
		SemValue value = CallResult(access.type);
		value.node->children.push_back(std::move(access.node));
		for (size_t i = 0; i < args.size(); i++)
			value.node->children.push_back(std::move(args[i].node));
		return value;
	}
	if (bare->kind == TK_CLASS)
		return AnalyzeFunctorCall(std::move(callable), expr, 1);
	TypePtr fn_type;
	if (bare->kind == TK_FUNCTION)
		fn_type = bare;
	else if (bare->kind == TK_POINTER &&
	         bare->target->kind == TK_FUNCTION)
		fn_type = bare->target;
	else
		throw runtime_error("__builtin_invoke callable type");
	vector<SemValue> args;
	AnalyzeArgumentList(expr.arguments, args, false, 1);
	CheckCallArguments(fn_type, args);
	SemValue value = CallResult(fn_type);
	value.node->children.push_back(std::move(callable.node));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

// __builtin_offsetof(type, designator): the summed member offsets of
// the resolved class layout (18.2: a constant of type size_t).
SemValue SemExprAnalyzer::AnalyzeOffsetof(const AstExpr& expr)
{
	TypePtr at = host_.ResolveCastTypeId(*expr.type);
	unsigned long long offset = 0;
	for (size_t step = 0; step < expr.arguments.size(); step++)
	{
		const AstExpr& item = *expr.arguments[step];
		TypePtr bare = RemoveTopCv(at);
		if (item.kind == EK_ID)
		{
			if (bare->kind != TK_CLASS)
				throw runtime_error("offsetof of a non-class member");
			host_.RequireCompleteType(bare->named);
			const ClassInfo* cls = host_.Classes().Find(bare->named);
			const ClassField* field =
				cls ? OffsetofField(*cls, item.name.parts[0].identifier,
				                    offset)
				    : 0;
			if (!field)
				throw runtime_error("offsetof member not found");
			if (field->is_bit_field)
				throw runtime_error("offsetof of a bit-field");
			at = field->type;
			continue;
		}
		if (bare->kind != TK_ARRAY)
			throw runtime_error("offsetof index into a non-array");
		ConstValue index;
		if (!host_.TryEvaluateConstant(item, index))
			throw runtime_error("offsetof index is not constant");
		offset += index.bits * TypeSize(RemoveTopCv(bare->target));
		at = bare->target;
	}
	SemValue value;
	value.type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = to_string(offset);
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	value.node->has_value = true;
	value.node->value = ConstValue(FT_UNSIGNED_LONG_INT, offset);
	return value;
}

// The named field row (direct or through a base subobject), with the
// traversed offset accumulated into `offset`.
const ClassField* SemExprAnalyzer::OffsetofField(const ClassInfo& cls,
                                                 const string& name,
                                                 unsigned long long& offset)
{
	for (size_t i = 0; i < cls.fields.size(); i++)
		if (cls.fields[i].name == name)
		{
			offset += cls.fields[i].offset;
			return &cls.fields[i];
		}
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		const ClassInfo* base = cls.direct_bases[b].cls;
		if (!base)
			continue;
		unsigned long long inner = offset + cls.direct_bases[b].offset;
		if (const ClassField* field = OffsetofField(*base, name, inner))
		{
			offset = inner;
			return field;
		}
	}
	return 0;
}
