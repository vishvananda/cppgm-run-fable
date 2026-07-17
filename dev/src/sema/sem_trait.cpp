#include "sema/sem_trait.h"

#include <stdexcept>

#include "sema/class_info.h"
#include "sema/const_eval.h"
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

namespace {

// The traits whose evaluation probes the initialization/conversion/
// assignment machinery (would-the-construct-compile semantics).
bool IsSemaProbeTraitName(const string& name)
{
	return name == "__is_constructible" ||
		name == "__is_nothrow_constructible" ||
		name == "__is_trivially_constructible" ||
		name == "__is_convertible" ||
		name == "__is_convertible_to" ||
		name == "__is_assignable" ||
		name == "__is_nothrow_assignable" ||
		name == "__is_trivially_assignable" ||
		name == "__is_destructible" ||
		name == "__is_nothrow_destructible" ||
		name == "__is_trivially_destructible" ||
		name == "__has_trivial_constructor" ||
		name == "__is_pod" ||
		name == "__is_trivial" ||
		name == "__is_trivially_copyable" ||
		name == "__is_standard_layout" ||
		name == "__is_literal_type" ||
		name == "__reference_constructs_from_temporary" ||
		name == "__reference_binds_to_temporary";
}

// A declval<A>() surrogate (20.2.4: A&& with reference collapse): an
// lvalue for lvalue-reference A, an xvalue otherwise. The node is
// never evaluated; probes only type-check against it.
SemValue DeclvalSurrogate(const TypePtr& type)
{
	SemValue value;
	TypePtr bare = type;
	if (type->kind == TK_LVALUE_REFERENCE)
	{
		bare = type->target;
		value.category = VC_LVALUE;
	}
	else if (type->kind == TK_RVALUE_REFERENCE)
	{
		bare = type->target;
		value.category = VC_XVALUE;
	}
	else
		value.category = VC_XVALUE;
	value.type = bare;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->type = bare;
	value.node->category = value.category;
	value.node->token = "0";
	return value;
}

}  // namespace

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
	bool value = IsSemaProbeTraitName(expr.op_spelling)
		? EvaluateSemaProbeTrait(expr.op_spelling, types)
		: EvaluateBuiltinTraitOnTypes(
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

// Dispatch for the would-it-compile trait family. Every probe runs in
// an unevaluated context; a probe failure (thrown diagnostic) reads as
// false, mirroring the substitution-failure semantics these traits
// model.
bool SemExprAnalyzer::EvaluateSemaProbeTrait(const string& name,
                                             const vector<TypePtr>& types)
{
	bool no_throw = false;
	bool trivial = false;
	if (name == "__is_constructible" ||
	    name == "__is_nothrow_constructible" ||
	    name == "__is_trivially_constructible" ||
	    name == "__has_trivial_constructor")
	{
		vector<TypePtr> args(types.begin() + 1, types.end());
		bool can = ProbeTraitConstructible(types[0], args, no_throw,
		                                   trivial);
		if (name == "__is_nothrow_constructible")
			return can && no_throw;
		if (name == "__is_trivially_constructible" ||
		    name == "__has_trivial_constructor")
			return can && trivial;
		return can;
	}
	if (name == "__is_convertible" || name == "__is_convertible_to")
		return ProbeTraitConvertible(types[0], types[1]);
	if (name == "__is_assignable" ||
	    name == "__is_nothrow_assignable" ||
	    name == "__is_trivially_assignable")
	{
		bool can = ProbeTraitAssignable(types[0], types[1], no_throw,
		                                trivial);
		if (name == "__is_nothrow_assignable")
			return can && no_throw;
		if (name == "__is_trivially_assignable")
			return can && trivial;
		return can;
	}
	if (name == "__is_destructible" ||
	    name == "__is_nothrow_destructible" ||
	    name == "__is_trivially_destructible")
	{
		bool can = ProbeTraitDestructible(types[0], no_throw, trivial);
		if (name == "__is_nothrow_destructible")
			return can && no_throw;
		if (name == "__is_trivially_destructible")
			return can && trivial;
		return can;
	}
	if (name == "__is_trivially_copyable" || name == "__is_trivial" ||
	    name == "__is_pod" || name == "__is_standard_layout" ||
	    name == "__is_literal_type")
	{
		TypePtr bare = RemoveTopCv(types[0]);
		while (bare->kind == TK_ARRAY)
			bare = RemoveTopCv(bare->target);
		if (bare->kind != TK_CLASS)
		{
			// Non-class categories: scalar types satisfy all five;
			// references satisfy only is_literal_type.
			if (IsReferenceType(bare))
				return name == "__is_literal_type";
			return bare->kind != TK_FUNCTION &&
				!(bare->kind == TK_FUNDAMENTAL &&
				  bare->fundamental == FT_VOID);
		}
		host_.RequireCompleteType(bare->named);
		const ClassInfo* cls = bare->named->class_record;
		if (!cls)
			return false;
		if (name == "__is_trivially_copyable")
			return ClassTriviallyCopyable(*cls);
		if (name == "__is_trivial")
			return ClassHasTrivialDefaultCtor(*cls) &&
				ClassTriviallyCopyable(*cls);
		if (name == "__is_standard_layout" || name == "__is_pod")
		{
			// Standard layout (9p7 approximation over the class
			// model): no virtual members or bases anywhere, no
			// reference members, and uniform access control over the
			// non-static data members.
			bool standard = !cls->is_polymorphic &&
				!ClassHasVBases(*cls);
			for (size_t i = 0; standard && i < cls->fields.size(); i++)
			{
				if (IsReferenceType(cls->fields[i].type) ||
				    cls->fields[i].access != cls->fields[0].access)
					standard = false;
			}
			if (name == "__is_standard_layout")
				return standard;
			return standard && ClassHasTrivialDefaultCtor(*cls) &&
				ClassTriviallyCopyable(*cls);
		}
		// __is_literal_type (3.9p10 approximation): trivial
		// destruction over the subobject tree.
		return ClassHasTrivialDtor(*cls);
	}
	if (name == "__reference_constructs_from_temporary" ||
	    name == "__reference_binds_to_temporary")
	{
		// 20.9.4.3: true when the reference initializes and binding
		// materializes a temporary (the operand is not
		// reference-compatible with the target).
		if (!IsReferenceType(types[0]))
			return false;
		vector<TypePtr> args(1, types[1]);
		bool can = ProbeTraitConstructible(types[0], args, no_throw,
		                                   trivial);
		if (!can)
			return false;
		TypePtr target = RemoveTopCv(types[0]->target);
		TypePtr source = types[1];
		if (IsReferenceType(source))
			source = source->target;
		source = RemoveTopCv(source);
		if (TypeEquals(target, source))
			return false;
		if (target->kind == TK_CLASS && source->kind == TK_CLASS)
		{
			int hops = 0;
			unsigned long long offset = 0;
			if (BaseSubobjectPath(source->named, target->named, hops,
			                      offset) != BP_NONE)
				return false;
		}
		return true;
	}
	throw runtime_error("unhandled sema probe trait: " + name);
}

// 20.9.4.3 is_constructible: would `T obj(declval<Args>()...)` (or the
// reference binding form) compile. Reports the selected path's
// noexcept and triviality facts alongside.
bool SemExprAnalyzer::ProbeTraitConstructible(const TypePtr& target,
                                              const vector<TypePtr>& arg_types,
                                              bool& no_throw, bool& trivial)
{
	no_throw = false;
	trivial = false;
	if (IsReferenceType(target))
	{
		if (arg_types.size() != 1)
			return false;
		bool saved = host_.SwapUnevaluatedOperand(true);
		try
		{
			SemValue value = DeclvalSurrogate(arg_types[0]);
			CopyInitialize(value, target, "trait probe");
			no_throw = !SemTreeMayThrow(*value.node);
			trivial = true;
			host_.SwapUnevaluatedOperand(saved);
			return true;
		}
		catch (const std::exception&)
		{
			host_.SwapUnevaluatedOperand(saved);
			return false;
		}
	}
	TypePtr bare = RemoveTopCv(target);
	// Arrays of known bound default-construct element-wise; any
	// argument (or an unknown bound) makes them non-constructible.
	while (bare->kind == TK_ARRAY)
	{
		if (!bare->bound_known || !arg_types.empty())
			return false;
		bare = RemoveTopCv(bare->target);
	}
	if (bare->kind == TK_FUNCTION ||
	    (bare->kind == TK_FUNDAMENTAL && bare->fundamental == FT_VOID))
		return false;
	if (bare->kind != TK_CLASS)
	{
		// Scalar targets: default/value-initialization or one
		// converting argument.
		if (arg_types.empty())
		{
			no_throw = true;
			trivial = true;
			return true;
		}
		if (arg_types.size() != 1)
			return false;
		bool saved = host_.SwapUnevaluatedOperand(true);
		try
		{
			SemValue value = DeclvalSurrogate(arg_types[0]);
			CopyInitialize(value, bare, "trait probe");
			no_throw = !SemTreeMayThrow(*value.node);
			trivial = true;
			host_.SwapUnevaluatedOperand(saved);
			return true;
		}
		catch (const std::exception&)
		{
			host_.SwapUnevaluatedOperand(saved);
			return false;
		}
	}
	host_.RequireCompleteType(bare->named);
	const ClassInfo* cls = host_.Classes().Find(bare->named);
	if (!cls)
		return false;
	bool saved = host_.SwapUnevaluatedOperand(true);
	try
	{
		vector<SemValue> args;
		for (size_t i = 0; i < arg_types.size(); i++)
			args.push_back(DeclvalSurrogate(arg_types[i]));
		int winner = host_.ResolveClassCtorHost(*cls, args, false,
		                                        "trait probe");
		vector<SemNodePtr> arg_nodes;
		for (size_t i = 0; i < args.size(); i++)
			arg_nodes.push_back(std::move(args[i].node));
		SemNodePtr call = host_.MakeConstructorCall(
			*cls, winner, false, SemNodePtr(), std::move(arg_nodes));
		no_throw = !SemTreeMayThrow(*call);
		if (winner >= 0)
		{
			const ClassCtor& ctor = cls->ctors[(size_t)winner];
			ECtorKind kind = ClassifyCtorKind(bare->named, ctor);
			if (arg_types.empty())
				trivial = ClassHasTrivialDefaultCtor(*cls);
			else if (kind == CK_COPY)
				trivial = ClassHasTrivialCopyCtor(*cls);
			else if (kind == CK_MOVE)
				trivial = ClassHasTrivialMoveCtor(*cls);
			else
				trivial = false;
		}
		host_.SwapUnevaluatedOperand(saved);
		return true;
	}
	catch (const std::exception&)
	{
		host_.SwapUnevaluatedOperand(saved);
		return false;
	}
}

// 20.9.6 is_convertible: would a copy-initialization of `to` from
// declval<from>() compile.
bool SemExprAnalyzer::ProbeTraitConvertible(const TypePtr& from,
                                            const TypePtr& to)
{
	bool from_void = from->kind == TK_FUNDAMENTAL &&
		from->fundamental == FT_VOID;
	bool to_void = to->kind == TK_FUNDAMENTAL &&
		to->fundamental == FT_VOID;
	if (from_void || to_void)
		return from_void && to_void;
	TypePtr bare_to = RemoveTopCv(to);
	if (bare_to->kind == TK_ARRAY || bare_to->kind == TK_FUNCTION)
		return false;
	bool saved = host_.SwapUnevaluatedOperand(true);
	try
	{
		SemValue value = DeclvalSurrogate(from);
		CopyInitialize(value, to, "trait probe");
		host_.SwapUnevaluatedOperand(saved);
		return true;
	}
	catch (const std::exception&)
	{
		host_.SwapUnevaluatedOperand(saved);
		return false;
	}
}

// 20.9.4.3 is_assignable: would `declval<Lhs>() = declval<Rhs>()`
// compile, with the selected path's noexcept/triviality facts.
bool SemExprAnalyzer::ProbeTraitAssignable(const TypePtr& lhs,
                                           const TypePtr& rhs,
                                           bool& no_throw, bool& trivial)
{
	no_throw = false;
	trivial = false;
	TypePtr left_type = IsReferenceType(lhs) ? lhs->target : lhs;
	bool left_lvalue = lhs->kind == TK_LVALUE_REFERENCE;
	TypePtr bare = RemoveTopCv(left_type);
	if (bare->kind == TK_CLASS)
	{
		host_.RequireCompleteType(bare->named);
		bool saved = host_.SwapUnevaluatedOperand(true);
		try
		{
			vector<SemValue> operands;
			operands.push_back(DeclvalSurrogate(lhs));
			operands.push_back(DeclvalSurrogate(rhs));
			SemValue result;
			if (!ResolveOperatorCall("=", operands, true, result))
			{
				host_.SwapUnevaluatedOperand(saved);
				return false;
			}
			no_throw = !SemTreeMayThrow(*result.node);
			const ClassInfo* cls = host_.Classes().Find(bare->named);
			TypePtr bare_rhs = rhs;
			if (IsReferenceType(bare_rhs))
				bare_rhs = bare_rhs->target;
			bare_rhs = RemoveTopCv(bare_rhs);
			if (cls && TypeEquals(bare, bare_rhs))
				trivial = rhs->kind == TK_LVALUE_REFERENCE
					? ClassHasTrivialCopyAssign(*cls)
					: ClassHasTrivialMoveAssign(*cls);
			host_.SwapUnevaluatedOperand(saved);
			return true;
		}
		catch (const std::exception&)
		{
			host_.SwapUnevaluatedOperand(saved);
			return false;
		}
	}
	// Scalar assignment: a modifiable scalar lvalue from a converting
	// operand (5.17p1).
	if (!left_lvalue || left_type->is_const ||
	    bare->kind == TK_ARRAY || bare->kind == TK_FUNCTION ||
	    (bare->kind == TK_FUNDAMENTAL && bare->fundamental == FT_VOID))
		return false;
	bool saved = host_.SwapUnevaluatedOperand(true);
	try
	{
		SemValue value = DeclvalSurrogate(rhs);
		CopyInitialize(value, bare, "trait probe");
		no_throw = true;
		trivial = true;
		host_.SwapUnevaluatedOperand(saved);
		return true;
	}
	catch (const std::exception&)
	{
		host_.SwapUnevaluatedOperand(saved);
		return false;
	}
}

// 20.9.4.3 is_destructible over the class model's destructor facts.
bool SemExprAnalyzer::ProbeTraitDestructible(const TypePtr& target,
                                             bool& no_throw, bool& trivial)
{
	no_throw = false;
	trivial = false;
	if (IsReferenceType(target))
	{
		no_throw = true;
		trivial = true;
		return true;
	}
	TypePtr bare = RemoveTopCv(target);
	while (bare->kind == TK_ARRAY)
	{
		if (!bare->bound_known)
			return false;
		bare = RemoveTopCv(bare->target);
	}
	if (bare->kind == TK_FUNCTION ||
	    (bare->kind == TK_FUNDAMENTAL && bare->fundamental == FT_VOID))
		return false;
	if (bare->kind != TK_CLASS)
	{
		no_throw = true;
		trivial = true;
		return true;
	}
	host_.RequireCompleteType(bare->named);
	const ClassInfo* cls = host_.Classes().Find(bare->named);
	if (!cls || cls->dtor_deleted)
		return false;
	trivial = ClassHasTrivialDtor(*cls);
	no_throw = trivial || cls->dtor_unwind_no ||
		(!cls->dtor_definition && cls->implicit_dtor_unwind_no);
	return true;
}
