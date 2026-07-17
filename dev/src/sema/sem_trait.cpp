#include "sema/sem_trait.h"

#include <stdexcept>

#include "sema/class_info.h"
#include "sema/sem_expr.h"

using std::runtime_error;
using std::string;
using std::vector;

// PA34 builtin type traits: __is_*/__has_* ( type-id-list ) evaluated
// over the semantic type model to a bool constant.

namespace {

// The class record behind a (possibly cv-qualified) class type, or
// null for every other type category.
const ClassInfo* ClassRecordOf(const TypePtr& type)
{
	if (type->kind != TK_CLASS || !type->named)
		return 0;
	return type->named->class_record;
}

bool TraitIsScalar(const TypePtr& type)
{
	// 3.9p9: arithmetic, enumeration, pointer, pointer-to-member, and
	// std::nullptr_t types (cv-qualified included).
	return IsArithmeticType(type) || type->kind == TK_ENUM ||
		type->kind == TK_POINTER || type->kind == TK_MEMBER_POINTER ||
		(type->kind == TK_FUNDAMENTAL &&
		 type->fundamental == FT_NULLPTR_T);
}

// 10.4p2: an abstract class has at least one pure virtual function
// without a non-pure final overrider.
bool TraitIsAbstract(const ClassInfo& cls)
{
	for (size_t i = 0; i < cls.vslots.size(); i++)
		if (cls.vslots[i].pure)
			return true;
	return false;
}

}  // namespace

bool EvaluateBuiltinTraitOnTypes(
	const string& name,
	const vector<TypePtr>& types,
	const std::function<void(const TypePtr&)>& require_complete)
{
	const TypePtr& first = types[0];
	if (name == "__is_same")
		return TypeEquals(first, types[1]);
	if (name == "__is_void")
		return first->kind == TK_FUNDAMENTAL &&
			first->fundamental == FT_VOID;
	if (name == "__is_integral")
		return IsIntegralType(first);
	if (name == "__is_floating_point")
		return first->kind == TK_FUNDAMENTAL &&
			IsFloatingFundamental(first->fundamental);
	if (name == "__is_arithmetic")
		return IsArithmeticType(first);
	if (name == "__is_signed")
		return first->kind == TK_FUNDAMENTAL &&
			(IsSignedIntegralFundamental(first->fundamental) ||
			 IsFloatingFundamental(first->fundamental));
	if (name == "__is_unsigned")
		return IsIntegralType(first) &&
			!IsSignedIntegralFundamental(first->fundamental);
	if (name == "__is_array")
		return first->kind == TK_ARRAY;
	if (name == "__is_pointer")
		return first->kind == TK_POINTER;
	if (name == "__is_reference")
		return IsReferenceType(first);
	if (name == "__is_lvalue_reference")
		return first->kind == TK_LVALUE_REFERENCE;
	if (name == "__is_rvalue_reference")
		return first->kind == TK_RVALUE_REFERENCE;
	if (name == "__is_function")
		return first->kind == TK_FUNCTION;
	if (name == "__is_member_pointer")
		return first->kind == TK_MEMBER_POINTER;
	if (name == "__is_member_object_pointer")
		return first->kind == TK_MEMBER_POINTER &&
			first->target->kind != TK_FUNCTION;
	if (name == "__is_member_function_pointer")
		return first->kind == TK_MEMBER_POINTER &&
			first->target->kind == TK_FUNCTION;
	if (name == "__is_enum")
		return first->kind == TK_ENUM;
	if (name == "__is_union")
	{
		if (first->kind != TK_CLASS)
			return false;
		require_complete(first);
		const ClassInfo* cls = ClassRecordOf(first);
		return cls && cls->is_union;
	}
	if (name == "__is_class")
	{
		if (first->kind != TK_CLASS)
			return false;
		const ClassInfo* cls = ClassRecordOf(first);
		// An incomplete class is still a class; unions need the record.
		return !cls || !cls->is_union;
	}
	if (name == "__is_scalar")
		return TraitIsScalar(first);
	if (name == "__is_empty" || name == "__is_final" ||
	    name == "__is_polymorphic" || name == "__is_abstract" ||
	    name == "__has_virtual_destructor")
	{
		if (first->kind != TK_CLASS)
			return false;
		require_complete(first);
		const ClassInfo* cls = ClassRecordOf(first);
		if (!cls)
			return false;
		if (name == "__is_empty")
			return cls->is_empty && !cls->is_union;
		if (name == "__is_final")
			return cls->is_final;
		if (name == "__is_polymorphic")
			return cls->is_polymorphic;
		if (name == "__is_abstract")
			return TraitIsAbstract(*cls);
		// __has_virtual_destructor
		return cls->dtor_virtual;
	}
	if (name == "__is_base_of")
	{
		// 20.9.6: classes only; a class is a base of itself. The
		// derived operand must be complete.
		if (first->kind != TK_CLASS || types[1]->kind != TK_CLASS)
			return false;
		if (first->named == types[1]->named)
			return true;
		require_complete(types[1]);
		int hops = 0;
		unsigned long long offset = 0;
		return BaseSubobjectPath(types[1]->named, first->named,
		                         hops, offset) != BP_NONE;
	}
	throw runtime_error("unknown builtin trait: " + name);
}

SemValue SemExprAnalyzer::AnalyzeBuiltinTrait(const AstExpr& expr)
{
	vector<TypePtr> types;
	for (size_t i = 0; i < expr.trait_args.size(); i++)
	{
		if (expr.trait_args[i].pack)
			throw runtime_error("pack-expanded builtin trait argument "
			                    "is not supported yet: " +
			                    expr.op_spelling);
		types.push_back(host_.ResolveCastTypeId(*expr.trait_args[i].type));
	}
	ISemExprHost& host = host_;
	bool value = EvaluateBuiltinTraitOnTypes(
		expr.op_spelling, types,
		[&host](const TypePtr& type) {
			host.RequireCompleteType(type->named);
		});
	SemValue out;
	out.type = MakeFundamentalType(FT_BOOL);
	out.category = VC_PRVALUE;
	out.node = MakeSemNode(SN_LITERAL);
	out.node->type = out.type;
	out.node->category = VC_PRVALUE;
	out.node->has_value = true;
	out.node->value = ConstValue(FT_BOOL, value ? 1 : 0);
	out.node->token = RenderConstValue(out.node->value);
	out.node->materialize_const = true;
	return out;
}
