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

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA34 assignment boundary");
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
	else if (name == "__builtin_complex")
		out = AnalyzeBuiltinComplex(expr);
	else if (name == "__builtin_FUNCTION")
		out = MakeFunctionNameLiteral(host_.CurrentFunctionName());
	else if ((name.compare(0, 13, "__c11_atomic_") == 0 ||
	          name.compare(0, 9, "__atomic_") == 0 ||
	          name.compare(0, 7, "__sync_") == 0) &&
	         name != "__c11_atomic_thread_fence" &&
	         name != "__c11_atomic_signal_fence")
		// The fences keep their fixed declarations
		// (ResolveBuiltinFunction).
		return TryAnalyzeAtomicBuiltin(expr, name, out);
	else
		return false;
	return true;
}

// The lock-free size queries fold from their constant size operand.
// x86-64: power-of-two sizes through 8 bytes are lock-free; 16
// answers false like host g++ without -mcx16 (and no 16-byte atomic
// codegen exists here to back a true).
SemValue SemExprAnalyzer::AnalyzeAtomicLockFreeQuery(const AstExpr& expr,
                                                     const string& name)
{
	TypePtr bool_type = MakeFundamentalType(FT_BOOL);
	if (expr.arguments.empty() || expr.arguments.size() > 2)
		throw runtime_error(name + " argument count");
	ConstValue size;
	if (!host_.TryEvaluateConstant(*expr.arguments[0], size))
		throw runtime_error(name + " requires a constant size");
	if (expr.arguments.size() > 1)
		Analyze(*expr.arguments[1]);
	bool lock_free = size.bits == 1 || size.bits == 2 ||
		size.bits == 4 || size.bits == 8;
	SemValue value;
	value.type = bool_type;
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = lock_free ? "true" : "false";
	value.node->type = bool_type;
	value.node->category = VC_PRVALUE;
	value.node->has_value = true;
	value.node->value = ConstValue(FT_BOOL, lock_free ? 1 : 0);
	return value;
}

// The GNU/C11 atomic operator families: each form's parameter list
// derives from the first argument's pointee type; the lowering maps
// them onto the LowIR atomic instructions.
bool SemExprAnalyzer::TryAnalyzeAtomicBuiltin(const AstExpr& expr,
                                              const string& name,
                                              SemValue& out)
{
	TypePtr int_type = MakeFundamentalType(FT_INT);
	TypePtr bool_type = MakeFundamentalType(FT_BOOL);
	TypePtr void_type = MakeFundamentalType(FT_VOID);
	if (name == "__atomic_always_lock_free" ||
	    name == "__atomic_is_lock_free" ||
	    name == "__c11_atomic_is_lock_free")
	{
		out = AnalyzeAtomicLockFreeQuery(expr, name);
		return true;
	}
	vector<SemValue> args;
	AnalyzeArgumentList(expr.arguments, args);
	if (args.empty())
		throw runtime_error(name + " needs an atomic address");
	TypePtr pointer = RemoveTopCv(args[0].type);
	if (pointer->kind != TK_POINTER)
		throw runtime_error(name + " requires a pointer operand");
	TypePtr element = RemoveTopCv(pointer->target);
	if (element->kind != TK_FUNDAMENTAL && element->kind != TK_POINTER &&
	    element->kind != TK_ENUM)
		throw runtime_error(name + " element type");
	vector<TypePtr> params;
	params.push_back(pointer);
	TypePtr result;
	if (name == "__c11_atomic_init")
	{
		params.push_back(element);
		result = void_type;
	}
	else if (name == "__c11_atomic_load" || name == "__atomic_load_n")
	{
		params.push_back(int_type);
		result = element;
	}
	else if (name == "__c11_atomic_store" || name == "__atomic_store_n")
	{
		params.push_back(element);
		params.push_back(int_type);
		result = void_type;
	}
	else if (name == "__atomic_load" || name == "__atomic_store")
	{
		if (args.size() < 2 ||
		    RemoveTopCv(args[1].type)->kind != TK_POINTER)
			throw runtime_error(name + " requires a value pointer");
		params.push_back(RemoveTopCv(args[1].type));
		params.push_back(int_type);
		result = void_type;
	}
	else if (name == "__c11_atomic_exchange" ||
	         name == "__atomic_exchange_n")
	{
		params.push_back(element);
		params.push_back(int_type);
		result = element;
	}
	else if (name == "__c11_atomic_compare_exchange_strong" ||
	         name == "__c11_atomic_compare_exchange_weak")
	{
		params.push_back(MakePointerType(element, false, false));
		params.push_back(element);
		params.push_back(int_type);
		params.push_back(int_type);
		result = bool_type;
	}
	else if (name == "__atomic_add_fetch" ||
	         name == "__atomic_sub_fetch" ||
	         name == "__atomic_fetch_add" ||
	         name == "__atomic_fetch_sub" ||
	         name == "__c11_atomic_fetch_add" ||
	         name == "__c11_atomic_fetch_sub" ||
	         name == "__c11_atomic_fetch_and" ||
	         name == "__c11_atomic_fetch_or" ||
	         name == "__c11_atomic_fetch_xor" ||
	         name == "__atomic_fetch_and" ||
	         name == "__atomic_fetch_or" ||
	         name == "__atomic_fetch_xor" ||
	         name == "__atomic_and_fetch" ||
	         name == "__atomic_or_fetch" ||
	         name == "__atomic_xor_fetch")
	{
		params.push_back(element);
		params.push_back(int_type);
		result = element;
	}
	else if (name == "__sync_lock_test_and_set")
	{
		params.push_back(element);
		result = element;
	}
	else if (name == "__sync_lock_release")
		result = void_type;
	else
		return false;
	TypePtr fn_type = MakeFunctionType(result, params, false);
	CheckCallArguments(fn_type, args);
	out = MakeBuiltinCallResult(name, fn_type, args, true);
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

// __builtin_{add,sub,mul}_overflow(a, b, out): each operand keeps its
// own promoted integer type; the lowering computes exactly in 128 bits
// and checks representability in the result object's type (the GCC
// infinite-precision semantics for every <= 64-bit combination).
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
	// The 128-bit widening scheme cannot check 128-bit values exactly;
	// that stays a loud boundary rather than a silent never-overflows.
	if (result->fundamental == FT_INT128 ||
	    result->fundamental == FT_UINT128)
		throw OutsideBoundary("128-bit overflow-builtin result");
	vector<TypePtr> params;
	for (size_t i = 0; i < 2; i++)
	{
		TypePtr promoted = PromoteForArithmetic(RemoveTopCv(args[i].type));
		if (!IsIntegralType(promoted) ||
		    promoted->fundamental == FT_BOOL)
			throw runtime_error(name + " requires integer operands");
		if (promoted->fundamental == FT_INT128 ||
		    promoted->fundamental == FT_UINT128)
			throw OutsideBoundary("128-bit overflow-builtin operand");
		params.push_back(promoted);
	}
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

// PA34 GNU __builtin_complex(re, im): the complex value of its parts;
// the value is the synthesized complex record initialized field-wise.
SemValue SemExprAnalyzer::AnalyzeBuiltinComplex(const AstExpr& expr)
{
	if (expr.arguments.size() != 2)
		throw runtime_error("__builtin_complex takes two arguments");
	vector<SemValue> args;
	AnalyzeArgumentList(expr.arguments, args);
	TypePtr element = RemoveTopCv(args[0].type);
	TypePtr other = RemoveTopCv(args[1].type);
	if (element->kind != TK_FUNDAMENTAL ||
	    !IsFloatingFundamental(element->fundamental) ||
	    !TypeEquals(element, other))
		throw runtime_error("__builtin_complex operands must be the "
		                    "same floating type");
	TypePtr complex_type = host_.MakeGnuComplexType(element);
	const ClassInfo* cls = host_.Classes().Find(complex_type->named);
	if (!cls)
		throw runtime_error("complex record missing");
	SemNodePtr action =
		host_.MakeAggregateTemporary(*cls, std::move(args));
	SemValue value;
	value.type = complex_type;
	value.category = VC_PRVALUE;
	action->type = complex_type;
	action->category = VC_PRVALUE;
	value.node = std::move(action);
	return value;
}

// GNU __real__/__imag__: a complex record selects its field; a real
// arithmetic operand is its own real part with a zero imaginary part.
SemValue SemExprAnalyzer::AnalyzeComplexPart(const AstExpr& expr)
{
	bool real = expr.op_spelling.compare(0, 6, "__real") == 0;
	SemValue operand = Analyze(*expr.operands[0]);
	TypePtr bare = RemoveTopCv(operand.type);
	if (bare->kind == TK_CLASS)
	{
		const ClassInfo* cls = host_.Classes().Find(bare->named);
		const ClassField* field = 0;
		if (cls)
			for (size_t i = 0; i < cls->fields.size(); i++)
				if (cls->fields[i].name ==
				    (real ? "__real" : "__imag"))
					field = &cls->fields[i];
		if (!field)
			throw runtime_error("__real__/__imag__ needs a complex "
			                    "operand");
		SemValue value;
		value.type = field->type;
		value.category = VC_LVALUE;
		value.node = MakeSemNode(SN_MEMBER_EXPRESSION);
		value.node->name = field->name;
		value.node->type = field->type;
		value.node->category = VC_LVALUE;
		value.node->member_offset = field->offset;
		value.node->children.push_back(std::move(operand.node));
		return value;
	}
	if (!IsArithmeticType(bare))
		throw runtime_error("__real__/__imag__ operand type");
	if (real)
		return operand;
	SemValue zero;
	zero.type = MakeFundamentalType(FT_INT);
	zero.category = VC_PRVALUE;
	zero.node = MakeSemNode(SN_LITERAL);
	zero.node->token = "0";
	zero.node->type = zero.type;
	zero.node->category = VC_PRVALUE;
	zero.node->has_value = true;
	zero.node->value = ConstValue(FT_INT, 0);
	CopyInitialize(zero, bare, "imaginary part");
	return zero;
}

// 20.8.2 INVOKE over the analyzed first argument: member function
// pointers bind their object argument, member data pointers read
// through it, anything else calls (function objects resolve their
// operator() / surrogates through the ordinary functor path).
SemValue SemExprAnalyzer::AnalyzeBuiltinInvoke(const AstExpr& expr)
{
	// Pack expansions among the arguments (`declval<Args>()...`)
	// expand before the INVOKE dispatch reads the callable.
	vector<SemValue> all;
	AnalyzeArgumentList(expr.arguments, all);
	if (all.empty())
		throw runtime_error("__builtin_invoke needs a callable");
	TypePtr bare = RemoveTopCv(all[0].type);
	if (bare->kind == TK_CLASS)
	{
		// The functor path resolves operator() over the analyzed
		// operand values (13.3.1.2 member selection).
		SemValue result;
		if (!ResolveOperatorCall("()", all, true, result))
			throw runtime_error("__builtin_invoke callable type");
		return result;
	}
	SemValue callable = std::move(all[0]);
	if (bare->kind == TK_MEMBER_POINTER)
	{
		if (all.size() < 2)
			throw runtime_error("__builtin_invoke member pointer "
			                    "needs an object");
		SemValue object = std::move(all[1]);
		bool arrow = RemoveTopCv(object.type)->kind == TK_POINTER;
		AstExpr access_shape(EK_BINARY);
		access_shape.op = arrow ? OP_ARROWSTAR : OP_DOTSTAR;
		access_shape.op_spelling = arrow ? "->*" : ".*";
		SemValue access = AnalyzeMemberPointerBinary(access_shape,
		                                             object, callable);
		vector<SemValue> args(
			std::make_move_iterator(all.begin() + 2),
			std::make_move_iterator(all.end()));
		if (access.type->kind != TK_FUNCTION)
		{
			if (!args.empty())
				throw runtime_error("__builtin_invoke data member "
				                    "takes one object");
			return access;
		}
		CheckCallArguments(access.type, args);
		SemValue value = CallResult(access.type);
		value.node->children.push_back(std::move(access.node));
		for (size_t i = 0; i < args.size(); i++)
			value.node->children.push_back(std::move(args[i].node));
		return value;
	}
	TypePtr fn_type;
	if (bare->kind == TK_FUNCTION)
		fn_type = bare;
	else if (bare->kind == TK_POINTER &&
	         bare->target->kind == TK_FUNCTION)
		fn_type = bare->target;
	else
		throw runtime_error("__builtin_invoke callable type");
	vector<SemValue> args(
		std::make_move_iterator(all.begin() + 1),
		std::make_move_iterator(all.end()));
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
