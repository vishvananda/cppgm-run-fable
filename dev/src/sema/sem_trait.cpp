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
		{
			vector<TypePtr> expanded;
			if (!host_.ExpandPackTypeId(*expr.trait_args[i].type,
			                            expanded))
				throw runtime_error("pack-expanded trait argument "
				                    "names no expandable pack: " +
				                    expr.op_spelling);
			types.insert(types.end(), expanded.begin(), expanded.end());
			continue;
		}
		types.push_back(host_.ResolveCastTypeId(*expr.trait_args[i].type));
	}
	if (types.empty())
		throw runtime_error("builtin trait needs at least one operand: " +
		                    expr.op_spelling);
	// PA34 value trait: __array_rank(T) counts array dimensions.
	if (expr.op_spelling == "__array_rank")
	{
		TypePtr bare = RemoveTopCv(types[0]);
		unsigned long long rank = 0;
		while (bare->kind == TK_ARRAY)
		{
			rank++;
			bare = RemoveTopCv(bare->target);
		}
		SemValue out;
		out.type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
		out.category = VC_PRVALUE;
		out.node = MakeSemNode(SN_LITERAL);
		out.node->type = out.type;
		out.node->category = VC_PRVALUE;
		out.node->has_value = true;
		out.node->value = ConstValue(FT_UNSIGNED_LONG_INT, rank);
		out.node->token = RenderConstValue(out.node->value);
		out.node->materialize_const = true;
		return out;
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
	if (name == "__is_invocable" || name == "__is_nothrow_invocable")
	{
		bool can = ProbeTraitInvocable(types, no_throw);
		return name == "__is_nothrow_invocable" ? can && no_throw
		                                        : can;
	}
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
		// The implicitly declared default constructor resolves as
		// winner -1; triviality follows the class facts either way.
		if (arg_types.empty())
			trivial = ClassHasTrivialDefaultCtor(*cls);
		else if (winner >= 0)
		{
			const ClassCtor& ctor = cls->ctors[(size_t)winner];
			ECtorKind kind = ClassifyCtorKind(bare->named, ctor);
			if (kind == CK_COPY)
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
	// A derived-to-base conversion is visible only once the pointee
	// class instantiates; incompleteness stays tolerated (identical
	// pointers convert regardless).
	for (int side = 0; side < 2; side++)
	{
		TypePtr pointee = RemoveTopCv(side ? to : from);
		while (pointee && (pointee->kind == TK_POINTER ||
		                   IsReferenceType(pointee)))
			pointee = RemoveTopCv(pointee->target);
		if (pointee && pointee->named &&
		    (pointee->kind == TK_CLASS ||
		     pointee->kind == TK_TEMPLATE_SPEC))
			try
			{
				host_.RequireCompleteType(pointee->named);
			}
			catch (const std::exception&)
			{
			}
	}
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

// 20.14 hosted invocable probe: would INVOKE(declval<F>(),
// declval<Args>()...) compile; `no_throw` reports the selected
// path's non-throwing specification.
bool SemExprAnalyzer::ProbeTraitInvocable(const vector<TypePtr>& types,
                                          bool& no_throw)
{
	no_throw = false;
	if (types.empty())
		return false;
	TypePtr bare = types[0];
	if (IsReferenceType(bare))
		bare = bare->target;
	bare = RemoveTopCv(bare);
	bool saved = host_.SwapUnevaluatedOperand(true);
	try
	{
		if (bare->kind == TK_CLASS)
		{
			host_.RequireCompleteType(bare->named);
			vector<SemValue> operands;
			for (size_t i = 0; i < types.size(); i++)
				operands.push_back(DeclvalSurrogate(types[i]));
			SemValue result;
			if (!ResolveOperatorCall("()", operands, true, result))
			{
				host_.SwapUnevaluatedOperand(saved);
				return false;
			}
			no_throw = !SemTreeMayThrow(*result.node);
			host_.SwapUnevaluatedOperand(saved);
			return true;
		}
		TypePtr fn;
		if (bare->kind == TK_FUNCTION)
			fn = bare;
		else if (bare->kind == TK_POINTER &&
		         bare->target->kind == TK_FUNCTION)
			fn = bare->target;
		if (!fn ||
		    types.size() - 1 < fn->parameters.size() ||
		    (types.size() - 1 > fn->parameters.size() &&
		     !fn->variadic))
		{
			// Member pointers stay a documented boundary.
			host_.SwapUnevaluatedOperand(saved);
			return false;
		}
		for (size_t i = 0; i + 1 < types.size(); i++)
			if (i < fn->parameters.size())
			{
				SemValue value = DeclvalSurrogate(types[i + 1]);
				CopyInitialize(value, fn->parameters[i],
				               "trait probe");
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

// PA34 fold-expressions, constant subset: the pattern expands over its
// packs and the elements combine as compile-time constants (the hosted
// sources use folds inside static_assert/enable_if conditions). A
// non-constant element is outside this stage's fold boundary.
SemValue SemExprAnalyzer::AnalyzeFold(const AstExpr& expr)
{
	const AstExpr* pattern = expr.operands[0].get();
	const AstExpr* init = 0;
	vector<SemValue> elements;
	if (expr.operands.size() == 2)
	{
		// Binary fold: exactly one side mentions the pack.
		if (host_.ExpandPackExpression(*expr.operands[0], elements))
			init = expr.operands[1].get();
		else
		{
			pattern = expr.operands[1].get();
			init = expr.operands[0].get();
			if (!host_.ExpandPackExpression(*pattern, elements))
				throw runtime_error("fold pattern names no expandable "
				                    "pack");
		}
	}
	else if (!host_.ExpandPackExpression(*pattern, elements))
		throw runtime_error("fold pattern names no expandable pack");

	if (expr.op != OP_LAND && expr.op != OP_LOR)
		throw runtime_error("fold operator " + expr.op_spelling +
		                    " is outside the constant fold subset");
	bool conjunction = expr.op == OP_LAND;
	bool result = conjunction;  // 14.5.3p9 empty-pack identity
	bool have_nonconstant = false;
	if (init)
	{
		SemValue value = Analyze(*init);
		RequireContextualBool(value, "fold operand");
		if (!value.node || !value.node->has_value)
			have_nonconstant = true;
		else if (conjunction)
			result = result && value.node->value.bits != 0;
		else
			result = result || value.node->value.bits != 0;
	}
	for (size_t i = 0; i < elements.size(); i++)
	{
		SemValue& value = elements[i];
		RequireContextualBool(value, "fold operand");
		if (!value.node || !value.node->has_value)
		{
			have_nonconstant = true;
			continue;
		}
		if (conjunction)
			result = result && value.node->value.bits != 0;
		else
			result = result || value.node->value.bits != 0;
	}
	// Short-circuit rescue: a non-constant element only matters when
	// the constant elements have not already decided the result.
	if (have_nonconstant && result == conjunction)
		throw runtime_error("non-constant fold element is outside the "
		                    "constant fold subset");
	SemValue out;
	out.type = MakeFundamentalType(FT_BOOL);
	out.category = VC_PRVALUE;
	out.node = MakeSemNode(SN_LITERAL);
	out.node->type = out.type;
	out.node->category = VC_PRVALUE;
	out.node->has_value = true;
	out.node->value = ConstValue(FT_BOOL, result ? 1 : 0);
	out.node->token = RenderConstValue(out.node->value);
	out.node->materialize_const = true;
	return out;
}
