#include "sema/sem_expr.h"

#include <stdexcept>

#include "ast/ast_text.h"
#include "sema/scope_lookup.h"

using std::runtime_error;
using std::to_string;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA12 assignment boundary");
}

bool IsIntegralOrUnscopedEnum(const TypePtr& type)
{
	return IsIntegralType(type) || IsUnscopedEnum(type);
}

bool IsPointerAfterDecay(const TypePtr& type)
{
	return type->kind == TK_POINTER || type->kind == TK_ARRAY;
}

TypePtr DecayToPointer(const TypePtr& type)
{
	if (type->kind == TK_ARRAY)
		return MakePointerType(type->target, false, false);
	return RemoveTopCv(type);
}

// 5.2.11p4 similarity for const_cast: the same type ignoring
// cv-qualification at every level.
bool SimilarTypes(const TypePtr& a, const TypePtr& b)
{
	TypePtr sa = RemoveTopCv(a);
	TypePtr sb = RemoveTopCv(b);
	if (sa->kind != sb->kind)
		return false;
	switch (sa->kind)
	{
	case TK_POINTER:
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
		return SimilarTypes(sa->target, sb->target);
	case TK_ARRAY:
		return sa->bound_known == sb->bound_known &&
			sa->bound == sb->bound &&
			SimilarTypes(sa->target, sb->target);
	default:
		return TypeEquals(sa, sb);
	}
}

}  // namespace

string CanonicalQualifiedName(const Scope* owner, const string& name)
{
	string path;
	for (const Scope* scope = owner; scope && scope->parent;
	     scope = scope->parent)
	{
		if (scope->kind == SCOPE_NAMESPACE && !scope->name.empty())
			path = scope->name + "::" + path;
	}
	return path + name;
}

ConversionSource MakeConversionSource(const SemValue& value)
{
	ConversionSource source;
	source.type = value.type;
	source.category = value.category;
	source.null_pointer_literal = value.null_pointer_literal;
	source.function_set = value.function_set;
	source.overloads = value.overloads;
	source.member_class = value.member_class;
	source.braced = value.braced_list;
	for (size_t i = 0; i < value.list_values.size(); i++)
		source.list_items.push_back(
			MakeConversionSource(value.list_values[i]));
	return source;
}

SemExprAnalyzer::SemExprAnalyzer(ISemExprHost& host)
	: host_(host)
{
}

SemValue SemExprAnalyzer::Analyze(const AstExpr& expr)
{
	switch (expr.kind)
	{
	case EK_LITERAL:
		return AnalyzeLiteral(expr);
	case EK_KEYWORD_LITERAL:
		return AnalyzeKeywordLiteral(expr);
	case EK_ID:
		return AnalyzeId(expr);
	case EK_PAREN:
		return Analyze(*expr.operands[0]);
	case EK_CALL:
		return AnalyzeCall(expr);
	case EK_UNARY:
		return AnalyzeUnary(expr);
	case EK_POSTFIX_INCDEC:
		return AnalyzeIncDec(expr, false);
	case EK_BINARY:
		return AnalyzeBinary(expr);
	case EK_ASSIGNMENT:
		return AnalyzeAssignment(expr);
	case EK_CONDITIONAL:
		return AnalyzeConditional(expr);
	case EK_SUBSCRIPT:
		return AnalyzeSubscript(expr);
	case EK_MEMBER:
		return AnalyzeMember(expr);
	case EK_THROW:
		return AnalyzeThrow(expr.operands.empty()
		                        ? 0 : expr.operands[0].get());
	case EK_DESIGNATED:
	{
		// PA34 `.name = value`: the value analyzes normally and
		// carries its member designator to the aggregate consumer.
		SemValue value = Analyze(*expr.operands[0]);
		value.designator = expr.name.parts[0].identifier;
		return value;
	}
	case EK_CSTYLE_CAST:
		// PA34 C compound literal `(T){...}`: the braced operand
		// list-initializes a temporary of T (the functional shape).
		if (expr.operands[0]->kind == EK_BRACED)
			return AnalyzeFunctionalCast(
				host_.ResolveCastTypeId(*expr.type),
				expr.operands[0]->arguments);
		return AnalyzeCastTo(host_.ResolveCastTypeId(*expr.type),
		                     *expr.operands[0], true, OP_LPAREN, "");
	case EK_KEYWORD_CAST:
		if (expr.op != KW_STATIC_CAST && expr.op != KW_CONST_CAST &&
		    expr.op != KW_REINTERPET_CAST &&
		    expr.op != KW_DYNAMIC_CAST)
			throw OutsideBoundary("cast keyword");
		return AnalyzeCastTo(host_.ResolveCastTypeId(*expr.type),
		                     *expr.operands[0], true, expr.op,
		                     expr.op_spelling);
	case EK_FUNCTIONAL_CAST:
	{
		AstTypeId keywords;
		for (size_t i = 0; i < expr.cast_keywords.size(); i++)
		{
			AstSpecifier spec;
			spec.kind = SPEC_KEYWORD;
			spec.keyword = expr.cast_keywords[i];
			keywords.specifiers.push_back(std::move(spec));
		}
		return AnalyzeFunctionalCast(host_.ResolveCastTypeId(keywords),
		                             expr.arguments);
	}
	case EK_SIZEOF_EXPR:
	case EK_SIZEOF_TYPE:
	case EK_TYPE_TRAIT:
		return AnalyzeSizeof(expr);
	case EK_BUILTIN_TRAIT:
		return AnalyzeBuiltinTrait(expr);
	case EK_FOLD:
		return AnalyzeFold(expr);
	case EK_COMPLEX_PART:
		return AnalyzeComplexPart(expr);
	case EK_COROUTINE_OP:
		// Parse-level hosted concession only: an instantiated
		// coroutine body stays outside the PA34 boundary.
		throw runtime_error("coroutine operator evaluation is outside "
		                    "the PA34 assignment boundary");
	case EK_SIZEOF_PACK:
	{
		// 5.3.3p5: the number of elements of the named pack; the value
		// materializes through a `const` instruction (the reference
		// shape).
		SemValue value;
		value.type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
		value.category = VC_PRVALUE;
		value.node = MakeSemNode(SN_LITERAL);
		value.node->type = value.type;
		value.node->category = VC_PRVALUE;
		value.node->has_value = true;
		value.node->value = ConstValue(
			FT_UNSIGNED_LONG_INT,
			host_.PackSize(expr.name.parts[0].identifier));
		value.node->token = RenderConstValue(value.node->value);
		value.node->materialize_const = true;
		return value;
	}
	case EK_NEW:
		return AnalyzeNew(expr);
	case EK_DELETE:
		return AnalyzeDelete(expr);
	case EK_LAMBDA:
		return host_.AnalyzeLambda(expr);
	case EK_STATEMENT_EXPR:
		return host_.AnalyzeStatementExpression(expr);
	case EK_VA_ARG:
		return AnalyzeVaArg(expr);
	case EK_OFFSETOF:
		return AnalyzeOffsetof(expr);
	default:
		throw runtime_error(
			"expression form kind " + to_string((int)expr.kind) +
			" is outside the PA12 assignment boundary");
	}
}

// PA33 __builtin_va_arg(list, T): the list operand decays to the
// register-cursor pointer; the fetch produces a prvalue T. The
// supported subset is the scalar classes (INTEGER, SSE f64, and the
// memory-class f80); class-typed fetches are later hosted-stage work.
SemValue SemExprAnalyzer::AnalyzeVaArg(const AstExpr& expr)
{
	// The operand stays in its analyzed form: an array-typed va_list
	// lvalue addresses directly at the lowering, a pointer-typed one
	// (a decayed parameter) loads its value there.
	SemValue list = Analyze(*expr.operands[0]);
	// The operand must be the va_list cursor (the unsigned-long-array
	// model or its decayed pointer); the lowering would read anything
	// else as a register cursor.
	TypePtr cursor = RemoveTopCv(
		IsReferenceType(list.type) ? list.type->target : list.type);
	TypePtr element;
	if (cursor && (cursor->kind == TK_ARRAY || cursor->kind == TK_POINTER))
		element = RemoveTopCv(cursor->target);
	if (!element || element->kind != TK_FUNDAMENTAL ||
	    element->fundamental != FT_UNSIGNED_LONG_INT)
		throw OutsideBoundary("va_arg cursor operand");
	TypePtr type = host_.ResolveCastTypeId(*expr.type);
	TypePtr bare = RemoveTopCv(type);
	if (bare->kind != TK_FUNDAMENTAL && bare->kind != TK_POINTER &&
	    bare->kind != TK_ENUM)
		throw OutsideBoundary("va_arg type class");
	SemValue value;
	value.type = bare;
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_VA_ARG);
	value.node->type = bare;
	value.node->category = VC_PRVALUE;
	value.node->typeid_operand = bare;
	value.node->children.push_back(std::move(list.node));
	return value;
}

SemValue SemExprAnalyzer::AnalyzeLiteral(const AstExpr& expr)
{
	SemValue value;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = expr.literal;
	if (expr.literal_kind == PTK_LITERAL)
	{
		value.type = MakeFundamentalType(expr.literal_type);
		value.category = VC_PRVALUE;
		value.null_pointer_literal =
			IsIntegralFundamental(expr.literal_type) &&
			LittleEndianValue(expr.literal_data) == 0;
		if (IsIntegralFundamental(expr.literal_type))
		{
			// Decoded value for the lowering: the stored 64 bits are
			// sign-extended for signed literal types.
			unsigned long long bits =
				LittleEndianValue(expr.literal_data);
			if (IsSignedIntegralFundamental(expr.literal_type))
			{
				size_t width = expr.literal_data.size() * 8;
				if (width < 64 && (bits >> (width - 1)) & 1)
					bits |= ~0ull << width;
			}
			value.node->has_value = true;
			value.node->value = ConstValue(expr.literal_type, bits);
			value.node->null_pointer = value.null_pointer_literal;
		}
	}
	else if (expr.literal_kind == PTK_LITERAL_ARRAY)
	{
		// 2.14.5p8: a string literal is an lvalue array of n const char.
		value.type = MakeArrayType(
			MakeCvQualifiedType(MakeFundamentalType(expr.literal_type),
			                    true, false),
			true, expr.literal_elements);
		value.category = VC_LVALUE;
		value.node->is_string_literal = true;
		value.node->string_bytes = expr.literal_data;
	}
	else if (expr.literal_kind == PTK_UD_STRING)
		return AnalyzeStringUdl(expr);
	else if (expr.literal_kind == PTK_UD_INTEGER)
		return AnalyzeNumericUdl(expr);
	else
		throw OutsideBoundary("user-defined literal");
	value.node->type = value.type;
	value.node->category = value.category;
	return value;
}

SemValue SemExprAnalyzer::AnalyzeKeywordLiteral(const AstExpr& expr)
{
	SemValue value;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = TokenTypeName(expr.op) + ":" + expr.literal;
	switch (expr.op)
	{
	case KW_TRUE:
	case KW_FALSE:
		value.type = MakeFundamentalType(FT_BOOL);
		value.node->has_value = true;
		value.node->value = ConstValue(FT_BOOL, expr.op == KW_TRUE);
		break;
	case KW_NULLPTR:
		value.type = MakeFundamentalType(FT_NULLPTR_T);
		value.node->null_pointer = true;
		break;
	case KW_THIS:
	{
		// 9.3.2: prvalue pointer to the cv-qualified class inside a
		// non-static member function. Inside a lambda body the value
		// reads the captured-this field (PA24 5.1.2).
		TypePtr this_type = host_.CurrentThisType();
		if (!this_type)
			throw runtime_error("this outside a member function");
		value.type = this_type;
		value.node = host_.ThisValueNode();
		break;
	}
	default:
		throw OutsideBoundary("this");
	}
	value.category = VC_PRVALUE;
	value.node->type = value.type;
	value.node->category = value.category;
	return value;
}

TypePtr SemExprAnalyzer::ThisAdjustedType(const NamedTypeInfo* cls,
                                          const TypePtr& member) const
{
	// The implicit object parameter spelled out: pointer to cv class
	// first, then the declared parameters; the trailing cv moves onto
	// the pointee.
	TypePtr class_type = MakeNamedType(TK_CLASS, cls);
	class_type = MakeCvQualifiedType(class_type, member->is_const,
	                                 member->is_volatile);
	vector<TypePtr> parameters;
	parameters.push_back(MakePointerType(class_type, false, false));
	for (size_t i = 0; i < member->parameters.size(); i++)
		parameters.push_back(member->parameters[i]);
	TypePtr adjusted = MakeFunctionType(member->target, parameters,
	                                    member->variadic);
	if (member->ref_qual)
		adjusted = MakeRefQualifiedType(adjusted, member->ref_qual);
	return adjusted;
}

// PA19 14.1p4: the constant prvalue behind an objectless binding (a
// non-type template parameter or variable-template specialization).
SemValue SemExprAnalyzer::FoldObjectlessConstant(const ScopeBinding& binding)
{
	if (!binding.has_value)
		throw runtime_error(binding.name +
		                    " is a dependent value parameter");
	SemValue value;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = RenderConstValue(binding.value);
	value.node->type = RemoveTopCv(binding.type);
	value.node->category = VC_PRVALUE;
	value.node->has_value = true;
	value.node->value = binding.value;
	value.type = value.node->type;
	value.category = VC_PRVALUE;
	return value;
}

// 9.3.1p3 with an explicit nested-name-specifier: whether a qualified
// field name inside a member function reads through this.
bool SemExprAnalyzer::QualifiedFieldThroughThis(
	const ScopeBinding& binding, const NamedTypeInfo* member_class)
{
	if (!member_class || binding.kind != SB_VARIABLE ||
	    !host_.CurrentThisType() ||
	    BaseClassDistance(host_.CurrentThisType()->target->named,
	                      member_class) < 0)
		return false;
	const ClassInfo* owner_cls = 0;
	if (const NamedTypeInfo* owner_entity =
	        host_.Model().ScopeEntity(binding.owner))
		owner_cls = host_.Classes().Find(owner_entity);
	return owner_cls && FindClassField(*owner_cls, binding.name) != 0;
}

// The SB_VARIABLE / SB_PARAMETER leg of an id-expression: folded
// constants, capture reads, static-member folds, and the member-fact
// stripping of static members. Returns true when `value` is complete;
// false falls through to the common id-node tail (with `member_class`
// possibly narrowed).
bool SemExprAnalyzer::AnalyzeVariableIdValue(const AstExpr& expr,
                                             const ScopeBinding& binding,
                                             const NamedTypeInfo*& member_class,
                                             const string& written,
                                             SemValue& value)
{
	// PA19: a non-type template parameter has no object behind it;
	// every use folds to its converted constant value (14.1p4).
	if (binding.no_object)
	{
		// PA26: a member-function pointer parameter re-forms the
		// `&C::f` constant over its member entity.
		TypePtr declared_pm = RemoveTopCv(binding.type);
		if (!binding.has_value && binding.param_index < 0 &&
		    declared_pm->kind == TK_MEMBER_POINTER &&
		    declared_pm->target->kind == TK_FUNCTION &&
		    binding.owner && binding.owner->kind == SCOPE_CLASS)
		{
			value = MemberPointerParamConstant(binding, declared_pm);
			return true;
		}
		value = FoldObjectlessConstant(binding);
		return true;
	}
	// PA24: an enclosing function-local used inside an open lambda
	// body reads through the closure's capture field.
	if (host_.TryCaptureUse(binding, value))
		return true;
	// A constant static member named through a qualified-id folds
	// like an enumerator (9.4.2p4 constant initializer). A
	// decltype-scoped qualified-id reads the entity itself (the
	// reference resolution keeps the odr-use).
	if (member_class && StaticMemberValueFolds(binding) &&
	    (expr.name.parts.empty() ||
	     expr.name.parts[0].kind != NP_DECLTYPE))
	{
		value = AnalyzeStaticMemberValue(binding, written);
		return true;
	}
	// PA26: an instantiated const function-pointer member's read
	// folds to its symbolic function address (the reference
	// presentation). The registered definition instantiates on
	// this demand before the fold decision.
	if (member_class && !binding.has_value && binding.type &&
	    binding.type->is_const && !binding.type->is_volatile &&
	    RemoveTopCv(binding.type)->kind == TK_POINTER &&
	    RemoveTopCv(binding.type)->target->kind == TK_FUNCTION)
		host_.OnStaticMemberReferenced(binding, false);
	if (member_class && binding.fn_pointer_fold)
	{
		value = AnalyzeStaticMemberValue(binding, written);
		return true;
	}
	// 5.3.1p3: only non-static data members carry the member
	// -pointer facts; a static member behaves as an ordinary
	// namespace-scope object.
	if (member_class)
	{
		const ClassInfo* owner_cls = 0;
		if (const NamedTypeInfo* owner_entity =
		        host_.Model().ScopeEntity(binding.owner))
			owner_cls = host_.Classes().Find(owner_entity);
		if (!owner_cls || !FindClassField(*owner_cls, binding.name))
			member_class = 0;
	}
	// 5p5: the expression type is the declared type with references
	// stripped; the result is an lvalue either way.
	TypePtr declared = binding.type;
	if (IsReferenceType(declared))
		declared = declared->target;
	value.type = declared;
	value.category = VC_LVALUE;
	value.member_class = member_class;
	value.member_type = declared;
	if (!binding.anon_storage_name.empty())
	{
		// Injected anonymous-union member (9.5p5): a synthesized
		// access through the storage variable.
		SemNodePtr storage = MakeSemNode(SN_ID_EXPRESSION);
		storage->name = binding.anon_storage_name;
		storage->type = binding.anon_storage_type;
		storage->category = VC_LVALUE;
		value.node = MakeSemNode(SN_MEMBER_EXPRESSION);
		value.node->name = written;
		value.node->type = declared;
		value.node->category = VC_LVALUE;
		value.node->children.push_back(std::move(storage));
		return true;
	}
	return false;
}

SemValue SemExprAnalyzer::AnalyzeId(const AstExpr& expr)
{
	const NamedTypeInfo* member_class = 0;
	// GNU __null: the null pointer constant NULL expands to (a
	// pointer-sized integer literal zero).
	if (expr.name.IsPlainIdentifier() &&
	    expr.name.parts[0].identifier == "__null")
	{
		SemValue value;
		value.type = MakeFundamentalType(FT_LONG_INT);
		value.category = VC_PRVALUE;
		value.null_pointer_literal = true;
		value.node = MakeSemNode(SN_LITERAL);
		value.node->token = "0";
		value.node->type = value.type;
		value.node->category = VC_PRVALUE;
		value.node->has_value = true;
		value.node->value = ConstValue(FT_LONG_INT, 0);
		value.node->null_pointer = true;
		return value;
	}
	// 8.4.1p8 / GNU: the predefined function-name variables (the GNU
	// pretty spelling approximates with the plain name; values are
	// not part of any grading contract).
	if (expr.name.IsPlainIdentifier() &&
	    (expr.name.parts[0].identifier == "__func__" ||
	     expr.name.parts[0].identifier == "__FUNCTION__" ||
	     expr.name.parts[0].identifier == "__PRETTY_FUNCTION__"))
	{
		// A real declaration of the name wins; otherwise the
		// predefined variable synthesizes here.
		try
		{
			host_.ResolveValue(expr.name, member_class);
		}
		catch (const std::exception&)
		{
			return MakeFunctionNameLiteral(host_.CurrentFunctionName());
		}
		member_class = 0;
	}
	const ScopeBinding* binding = host_.ResolveValue(expr.name, member_class);
	string written = FlattenNameTopLevel(expr.name);
	// An unqualified name found in a class scope is an implicit member
	// use (9.3.1p3); qualified uses keep the PA12 member facts.
	if (!member_class && binding->home &&
	    binding->home->kind == SCOPE_CLASS &&
	    (binding->kind == SB_VARIABLE || binding->kind == SB_FUNCTION))
		return AnalyzeImplicitMember(*binding, written);
	// A qualified field name inside a member function also reads
	// through this (9.3.1p3 with an explicit nested-name-specifier).
	if (QualifiedFieldThroughThis(*binding, member_class))
		return AnalyzeImplicitMember(*binding, written);
	SemValue value;
	switch (binding->kind)
	{
	case SB_VARIABLE:
	case SB_PARAMETER:
		if (AnalyzeVariableIdValue(expr, *binding, member_class,
		                           written, value))
			return value;
		break;
	case SB_FUNCTION:
		// 9.4p2: a set of only static member functions behaves as
		// ordinary functions (its address is a plain function
		// pointer, not a member pointer).
		if (member_class && FunctionSetAllStatic(*binding))
			member_class = 0;
		FillFunctionSetValue(*binding, member_class, value);
		// 14.8.1: a template-id that could not resolve fully keeps its
		// explicit arguments for target-directed deduction.
		if (!binding->fn_self_spec && !expr.name.parts.empty() &&
		    expr.name.parts.back().kind == NP_TEMPLATE_ID)
			value.fn_explicit_part = &expr.name.parts.back();
		break;
	case SB_ENUMERATOR:
	{
		// Enumerators dump as value literals of their enumeration type.
		value.node = MakeSemNode(SN_LITERAL);
		value.node->token = RenderConstValue(binding->value);
		value.node->type = binding->type;
		value.node->category = VC_PRVALUE;
		value.node->has_value = true;
		value.node->value = binding->value;
		value.type = binding->type;
		value.category = VC_PRVALUE;
		return value;
	}
	default:
		throw runtime_error(binding->name +
		                    " does not name a value in an expression");
	}
	if (binding->kind == SB_VARIABLE && !binding->has_value &&
	    binding->owner && binding->owner->kind == SCOPE_CLASS)
		host_.OnStaticMemberReferenced(*binding, false);
	value.node = MakeSemNode(SN_ID_EXPRESSION);
	value.node->name = written;
	value.node->type = value.type;
	value.node->category = value.category;
	value.node->entity_scope = binding->owner;
	// PA19: a pack-element binding reads/writes its expanded slot.
	value.node->entity_name = binding->pack_element_name.empty()
		? binding->name : binding->pack_element_name;
	value.node->fn_spec = binding->fn_self_spec;
	host_.OnSpecializationOdrUsed(binding->fn_self_spec);
	return value;
}

// PA21 9.4p2: whether every entry of a member function set (ordinary
// overloads and member templates alike) is static.
bool SemExprAnalyzer::FunctionSetAllStatic(const ScopeBinding& binding)
{
	bool any_entry = false;
	size_t ordinary = binding.type ? binding.overloads.size() + 1 : 0;
	for (size_t i = 0; i < ordinary; i++)
	{
		any_entry = true;
		if (i >= binding.fn_static.size() || !binding.fn_static[i])
			return false;
	}
	for (size_t t = 0; t < binding.fn_templates.size(); t++)
	{
		any_entry = true;
		if (!binding.fn_templates[t]->member_static)
			return false;
	}
	return any_entry;
}

// An id-expression naming a function overload set (possibly one
// deduced specialization): the set's facts ride on the value until a
// call or a target type selects one member.
void SemExprAnalyzer::FillFunctionSetValue(const ScopeBinding& binding,
                                           const NamedTypeInfo* member_class,
                                           SemValue& value)
{
	value.function_set = true;
	if (binding.type)
	{
		value.overloads.push_back(binding.type);
		for (size_t i = 0; i < binding.overloads.size(); i++)
			value.overloads.push_back(binding.overloads[i]);
	}
	value.fn_templates = binding.fn_templates;
	value.overload_specs.resize(value.overloads.size(), 0);
	if (binding.fn_self_spec && !value.overload_specs.empty())
		value.overload_specs[0] = binding.fn_self_spec;
	value.fn_owner = binding.owner;
	value.fn_name = binding.name;
	value.category = VC_LVALUE;
	value.member_class = member_class;
	value.member_type = binding.type;
	value.type = member_class
		? ThisAdjustedType(member_class, binding.type)
		: binding.type;
}

SemValue SemExprAnalyzer::CallResult(const TypePtr& function_type)
{
	SemValue value;
	const TypePtr& result = function_type->target;
	value.node = MakeSemNode(SN_CALL_EXPRESSION);
	value.node->type = result;
	// 12.2: a class-valued result is a destructible temporary at its
	// materialization point (destroyed even when the chain is
	// effect-free); the resolved destructor pins on the node.
	if (!IsReferenceType(result) && RemoveTopCv(result)->kind == TK_CLASS)
	{
		// The called function's class result is complete at the call
		// (5.2.2p3); a class-template result instantiates here so the
		// temporary's destructor resolves.
		host_.RequireCompleteType(RemoveTopCv(result)->named);
		if (const ClassInfo* cls =
		        host_.Classes().Find(RemoveTopCv(result)->named))
			if (host_.Classes().NeedsDestruction(*cls))
			{
				value.node->needs_dtor = true;
				value.node->result_dtor = host_.MakeTemporaryDtor(*cls);
			}
	}
	if (result->kind == TK_LVALUE_REFERENCE)
	{
		value.category = VC_LVALUE;
		value.type = result->target;
	}
	else if (result->kind == TK_RVALUE_REFERENCE)
	{
		value.category = VC_XVALUE;
		value.type = result->target;
	}
	else
	{
		value.category = VC_PRVALUE;
		value.type = result;
	}
	value.node->category = value.category;
	return value;
}

// --- calls ---------------------------------------------------------------





SemValue SemExprAnalyzer::AnalyzeBuiltinConstantP(const AstExpr& expr)
{
	if (expr.arguments.size() != 1)
		throw runtime_error("__builtin_constant_p takes one argument");
	Analyze(*expr.arguments[0]);  // the operand must still be valid
	ConstValue ignored;
	bool constant = host_.TryEvaluateConstant(*expr.arguments[0], ignored);
	SemValue value;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = constant ? "1" : "0";
	value.type = MakeFundamentalType(FT_INT);
	value.category = VC_PRVALUE;
	value.node->type = value.type;
	value.node->category = value.category;
	value.node->has_value = true;
	value.node->value = ConstValue(FT_INT, constant ? 1 : 0);
	return value;
}

// --- unary ----------------------------------------------------------------

SemValue SemExprAnalyzer::AnalyzeAddressOf(const AstExpr& expr)
{
	SemValue operand = Analyze(*expr.operands[0]);
	if (!operand.member_class || !operand.function_set)
	{
		SemValue overloaded;
		if (TryUnaryOperator("&", operand, false, overloaded))
			return overloaded;
	}
	SemValue value;
	if (operand.member_class)
	{
		// PA26 13.4: an overloaded (or template) member set under &
		// stays unresolved; the member pointer target type selects the
		// overload (5.3.1p4 with 14.8.2.2).
		if (operand.function_set &&
		    (operand.overloads.size() > 1 ||
		     !operand.fn_templates.empty()))
		{
			operand.fn_set_addressed = true;
			return operand;
		}
		// 5.3.1p3-p4: &C::member forms a member pointer over the
		// declared (cv-qualified) member type.
		value.type = MakeMemberPointerType(operand.member_class,
		                                   operand.member_type,
		                                   false, false);
	}
	else if (operand.function_set)
	{
		// 13.4: a set whose template members are still undeduced stays
		// unresolved under &; the target type selects the member and
		// forms the pointer there (14.8.2.2).
		if (!operand.fn_templates.empty() && operand.overloads.empty())
		{
			operand.fn_set_addressed = true;
			return operand;
		}
		if (operand.overloads.size() > 1)
			throw OutsideBoundary("address of an overloaded name");
		value.type = MakePointerType(operand.type, false, false);
	}
	else
	{
		if (operand.category != VC_LVALUE)
			throw runtime_error("address of a non-lvalue");
		value.type = MakePointerType(operand.type, false, false);
	}
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_UNARY_EXPRESSION);
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	value.node->has_op = true;
	value.node->op = expr.op;
	value.node->op_spelling = expr.op_spelling;
	value.node->children.push_back(std::move(operand.node));
	return value;
}

SemValue SemExprAnalyzer::AnalyzeIncDec(const AstExpr& expr, bool prefix)
{
	SemValue operand = Analyze(*expr.operands[0]);
	{
		SemValue overloaded;
		if (TryUnaryOperator(expr.op_spelling, operand, !prefix,
		                     overloaded))
			return overloaded;
	}
	// 13.3.1.2p3 with 13.6: no member operator - a class operand may
	// still reach the built-in form through a conversion function
	// yielding a reference to arithmetic.
	if (RemoveTopCv(operand.type)->kind == TK_CLASS)
		ConvertClassOperand(operand);
	RequireModifiableLvalue(operand, "increment or decrement");
	bool arithmetic = IsArithmeticType(operand.type) &&
		!(operand.type->kind == TK_FUNDAMENTAL &&
		  operand.type->fundamental == FT_BOOL && expr.op == OP_DEC);
	if (!arithmetic && !IsObjectPointer(operand.type))
		throw runtime_error("invalid increment or decrement operand");
	SemValue value;
	value.type = RemoveTopCv(operand.type);
	value.category = prefix ? VC_LVALUE : VC_PRVALUE;
	value.node = MakeSemNode(prefix ? SN_UNARY_EXPRESSION
	                                : SN_POSTFIX_EXPRESSION);
	value.node->type = value.type;
	value.node->category = value.category;
	value.node->has_op = true;
	value.node->op = expr.op;
	value.node->op_spelling = expr.op_spelling;
	value.node->children.push_back(std::move(operand.node));
	return value;
}

SemValue SemExprAnalyzer::AnalyzeUnary(const AstExpr& expr)
{
	if (expr.op == OP_AMP)
		return AnalyzeAddressOf(expr);
	if (expr.op == OP_INC || expr.op == OP_DEC)
		return AnalyzeIncDec(expr, true);

	SemValue operand = Analyze(*expr.operands[0]);
	{
		SemValue overloaded;
		if (TryUnaryOperator(expr.op_spelling, operand, false,
		                     overloaded))
			return overloaded;
	}
	SemValue value;
	switch (expr.op)
	{
	case OP_STAR:
	{
		// 5.3.1p1 over pointers; arrays decay first, so *a is the
		// first element and *fp is the function lvalue.
		TypePtr pointee;
		if (operand.type->kind == TK_ARRAY)
			pointee = operand.type->target;
		else if (operand.type->kind == TK_POINTER)
			pointee = operand.type->target;
		else
			throw runtime_error("indirection requires a pointer");
		value.type = pointee;
		value.category = VC_LVALUE;
		break;
	}
	case OP_LNOT:
		RequireContextualBool(operand, "operand of !");
		value.type = MakeFundamentalType(FT_BOOL);
		value.category = VC_PRVALUE;
		break;
	case OP_PLUS:
		if (IsPointerAfterDecay(operand.type))
			value.type = DecayToPointer(operand.type);
		else
			value.type = PromoteForArithmetic(operand.type);
		if (!IsArithmeticType(value.type) && value.type->kind != TK_POINTER)
			throw runtime_error("invalid operand of unary +");
		value.category = VC_PRVALUE;
		break;
	case OP_MINUS:
		value.type = PromoteForArithmetic(operand.type);
		if (!IsArithmeticType(value.type))
			throw runtime_error("invalid operand of unary -");
		value.category = VC_PRVALUE;
		break;
	case OP_COMPL:
		if (!IsIntegralOrUnscopedEnum(operand.type))
			throw runtime_error("invalid operand of ~");
		value.type = PromoteForArithmetic(operand.type);
		value.category = VC_PRVALUE;
		break;
	default:
		throw OutsideBoundary("unary operator");
	}
	value.node = MakeSemNode(SN_UNARY_EXPRESSION);
	value.node->type = value.type;
	value.node->category = value.category;
	value.node->has_op = true;
	value.node->op = expr.op;
	value.node->op_spelling = expr.op_spelling;
	value.node->children.push_back(std::move(operand.node));
	return value;
}

// --- binary ----------------------------------------------------------------

SemValue SemExprAnalyzer::MakeBinaryNode(const AstExpr& expr, SemValue& lhs,
                                         SemValue& rhs,
                                         EValueCategory category,
                                         const TypePtr& type)
{
	SemValue value;
	value.type = type;
	value.category = category;
	value.node = MakeSemNode(SN_BINARY_EXPRESSION);
	value.node->type = type;
	value.node->category = category;
	value.node->has_op = true;
	value.node->op = expr.op;
	value.node->op_spelling = expr.op_spelling;
	value.node->children.push_back(std::move(lhs.node));
	value.node->children.push_back(std::move(rhs.node));
	return value;
}

SemValue SemExprAnalyzer::AnalyzeAdditive(const AstExpr& expr, SemValue& lhs,
                                          SemValue& rhs)
{
	bool left_pointer = IsPointerAfterDecay(lhs.type);
	bool right_pointer = IsPointerAfterDecay(rhs.type);
	if (left_pointer && right_pointer)
	{
		// 5.7p2: pointer subtraction over compatible object pointees.
		TypePtr a = DecayToPointer(lhs.type);
		TypePtr b = DecayToPointer(rhs.type);
		if (expr.op != OP_MINUS || !IsObjectPointer(a) ||
		    !IsObjectPointer(b) ||
		    !TypeEquals(RemoveTopCv(a->target), RemoveTopCv(b->target)))
			throw runtime_error("invalid pointer arithmetic");
		return MakeBinaryNode(expr, lhs, rhs, VC_PRVALUE,
		                      MakeFundamentalType(FT_LONG_INT));
	}
	if (left_pointer || right_pointer)
	{
		SemValue& pointer = left_pointer ? lhs : rhs;
		SemValue& index = left_pointer ? rhs : lhs;
		TypePtr decayed = DecayToPointer(pointer.type);
		if (!IsObjectPointer(decayed) ||
		    !IsIntegralOrUnscopedEnum(index.type) ||
		    (expr.op == OP_MINUS && !left_pointer))
			throw runtime_error("invalid pointer arithmetic");
		return MakeBinaryNode(expr, lhs, rhs, VC_PRVALUE, decayed);
	}
	return MakeBinaryNode(expr, lhs, rhs, VC_PRVALUE,
	                      UsualArithmeticConversions(lhs.type, rhs.type));
}

SemValue SemExprAnalyzer::AnalyzeComparison(const AstExpr& expr, SemValue& lhs,
                                            SemValue& rhs)
{
	bool equality = expr.op == OP_EQ || expr.op == OP_NE;
	// PA26 5.10p2: member pointers compare for (in)equality with each
	// other and with null pointer constants.
	bool left_pm = RemoveTopCv(lhs.type)->kind == TK_MEMBER_POINTER;
	bool right_pm = RemoveTopCv(rhs.type)->kind == TK_MEMBER_POINTER;
	if (left_pm || right_pm)
	{
		if (!equality)
			throw runtime_error("relational member pointer comparison");
		bool left_ok = left_pm || lhs.null_pointer_literal ||
			IsNullPtrType(lhs.type);
		bool right_ok = right_pm || rhs.null_pointer_literal ||
			IsNullPtrType(rhs.type);
		if (!left_ok || !right_ok ||
		    (left_pm && right_pm &&
		     !TypeEquals(RemoveTopCv(lhs.type), RemoveTopCv(rhs.type))))
			throw runtime_error("member pointer comparison mismatch");
		return MakeBinaryNode(expr, lhs, rhs, VC_PRVALUE,
		                      MakeFundamentalType(FT_BOOL));
	}
	bool left_pointer = IsPointerAfterDecay(lhs.type) ||
		IsNullPtrType(lhs.type);
	bool right_pointer = IsPointerAfterDecay(rhs.type) ||
		IsNullPtrType(rhs.type);
	if (left_pointer || right_pointer)
	{
		bool left_null = lhs.null_pointer_literal || IsNullPtrType(lhs.type);
		bool right_null = rhs.null_pointer_literal ||
			IsNullPtrType(rhs.type);
		if ((IsNullPtrType(lhs.type) || IsNullPtrType(rhs.type)) &&
		    !equality)
			throw runtime_error("relational comparison with nullptr");
		if (left_pointer && right_pointer && !left_null && !right_null)
		{
			if (!CompositePointerType(lhs, rhs))
				throw runtime_error("comparison of incompatible pointers");
		}
		else if (!(left_null || right_null))
			throw runtime_error("comparison of pointer and non-pointer");
	}
	else
	{
		// Arithmetic and enumerations compare through the usual
		// arithmetic conversions (scoped enums only with themselves).
		if (lhs.type->kind == TK_ENUM && lhs.type->named->is_scoped)
		{
			if (!TypeEquals(RemoveTopCv(lhs.type), RemoveTopCv(rhs.type)))
				throw runtime_error("scoped enum comparison mismatch");
		}
		else
			UsualArithmeticConversions(lhs.type, rhs.type);
	}
	return MakeBinaryNode(expr, lhs, rhs, VC_PRVALUE,
	                      MakeFundamentalType(FT_BOOL));
}

SemValue SemExprAnalyzer::AnalyzeBinary(const AstExpr& expr)
{
	SemValue lhs = Analyze(*expr.operands[0]);
	SemValue rhs = Analyze(*expr.operands[1]);
	{
		SemValue overloaded;
		if (TryBinaryOperator(expr.op_spelling, lhs, rhs, overloaded))
			return overloaded;
	}
	// A function set no candidate parameter resolved has no type; the
	// built-in forms below cannot take it (13.6 lists no such operand).
	if (!lhs.type || !rhs.type)
		throw runtime_error("unresolved overloaded function operand");
	// 13.6: class operands reach the built-in forms through their
	// conversion functions once user-declared operators are rejected.
	if (lhs.type->kind == TK_CLASS)
		ConvertClassOperand(lhs);
	if (rhs.type->kind == TK_CLASS)
		ConvertClassOperand(rhs);
	switch (expr.op)
	{
	case OP_PLUS:
	case OP_MINUS:
		return AnalyzeAdditive(expr, lhs, rhs);
	case OP_STAR:
	case OP_DIV:
		return MakeBinaryNode(
			expr, lhs, rhs, VC_PRVALUE,
			UsualArithmeticConversions(lhs.type, rhs.type));
	case OP_MOD:
	case OP_AMP:
	case OP_BOR:
	case OP_XOR:
		if (!IsIntegralOrUnscopedEnum(lhs.type) ||
		    !IsIntegralOrUnscopedEnum(rhs.type))
			throw runtime_error("integral operands required");
		return MakeBinaryNode(
			expr, lhs, rhs, VC_PRVALUE,
			UsualArithmeticConversions(lhs.type, rhs.type));
	case OP_LSHIFT:
	case OP_RSHIFT:
		if (!IsIntegralOrUnscopedEnum(lhs.type) ||
		    !IsIntegralOrUnscopedEnum(rhs.type))
			throw runtime_error("integral operands required");
		// 5.8p1: the result type is the promoted left operand.
		return MakeBinaryNode(expr, lhs, rhs, VC_PRVALUE,
		                      PromoteForArithmetic(lhs.type));
	case OP_LT:
	case OP_GT:
	case OP_LE:
	case OP_GE:
	case OP_EQ:
	case OP_NE:
		return AnalyzeComparison(expr, lhs, rhs);
	case OP_LAND:
	case OP_LOR:
		RequireContextualBool(lhs, "logical operand");
		RequireContextualBool(rhs, "logical operand");
		return MakeBinaryNode(expr, lhs, rhs, VC_PRVALUE,
		                      MakeFundamentalType(FT_BOOL));
	case OP_COMMA:
		return MakeBinaryNode(expr, lhs, rhs, rhs.category, rhs.type);
	case OP_DOTSTAR:
	case OP_ARROWSTAR:
		return AnalyzeMemberPointerBinary(expr, lhs, rhs);
	default:
		throw OutsideBoundary("binary operator");
	}
}

SemValue SemExprAnalyzer::AnalyzeAssignment(const AstExpr& expr)
{
	SemValue lhs = Analyze(*expr.operands[0]);
	const AstExpr& right = *expr.operands[1];
	SemValue rhs;
	bool braced_assign_list = false;
	if (right.kind == EK_BRACED && expr.op == OP_ASS &&
	    lhs.type->kind == TK_CLASS)
	{
		// PA25 13.5.3: when the class declares an operator= over
		// std::initializer_list, the braced operand stays a list for
		// overload resolution (5.17p9 otherwise builds the temporary).
		const NamedTypeInfo* named = RemoveTopCv(lhs.type)->named;
		vector<const Scope*> worklist;
		if (const Scope* members = host_.Model().MemberScope(named))
			worklist.push_back(members);
		for (size_t w = 0; w < worklist.size() && !braced_assign_list;
		     w++)
		{
			const Scope* link = worklist[w];
			if (link->class_base)
				worklist.push_back(link->class_base);
			for (size_t b = 0; b < link->class_extra_bases.size(); b++)
				worklist.push_back(link->class_extra_bases[b]);
			if (const ScopeBinding* assign =
			        FindOwnBinding(*link, "operator ="))
			{
				vector<TypePtr> types;
				if (assign->type)
					types.push_back(assign->type);
				for (size_t i = 0; i < assign->overloads.size(); i++)
					types.push_back(assign->overloads[i]);
				for (size_t i = 0; i < types.size(); i++)
					if (types[i] && types[i]->kind == TK_FUNCTION &&
					    !types[i]->parameters.empty() &&
					    IsStdInitializerList(types[i]->parameters[0],
					                         0))
						braced_assign_list = true;
			}
		}
		if (braced_assign_list)
		{
			rhs.braced_list = true;
			AnalyzeArgumentList(right.arguments, rhs.list_values);
		}
		else
			// 5.17p9: a braced-init-list right operand
			// list-initializes a temporary of the left operand's
			// class type.
			rhs = MakeTemporaryObject(RemoveTopCv(lhs.type),
			                          right.arguments, true);
	}
	else
		rhs = Analyze(right);
	if (lhs.type->kind == TK_CLASS || lhs.type->kind == TK_ENUM)
	{
		// 13.5.3: operator= must be a member; compound forms also
		// consider namespace-scope operators.
		vector<SemValue> operands;
		operands.push_back(std::move(lhs));
		operands.push_back(std::move(rhs));
		SemValue overloaded;
		if (ResolveOperatorCall(expr.op_spelling, operands,
		                        expr.op == OP_ASS, overloaded))
			return overloaded;
		lhs = std::move(operands[0]);
		rhs = std::move(operands[1]);
	}
	RequireModifiableLvalue(lhs, "assignment");
	if (lhs.type->kind == TK_CLASS)
		throw OutsideBoundary("class assignment");
	if (expr.op != OP_ASS && rhs.type->kind == TK_CLASS)
		ConvertClassOperand(rhs);
	if (expr.op == OP_ASS)
		CopyInitialize(rhs, lhs.type, "assignment");
	else if (lhs.type->kind == TK_POINTER)
	{
		if ((expr.op != OP_PLUSASS && expr.op != OP_MINUSASS) ||
		    !IsObjectPointer(lhs.type) ||
		    !IsIntegralOrUnscopedEnum(rhs.type))
			throw runtime_error("invalid pointer compound assignment");
	}
	else if (expr.op == OP_PLUSASS || expr.op == OP_MINUSASS ||
	         expr.op == OP_STARASS || expr.op == OP_DIVASS)
	{
		if (!IsArithmeticType(lhs.type) ||
		    !(IsArithmeticType(rhs.type) || IsUnscopedEnum(rhs.type)))
			throw runtime_error("invalid compound assignment");
	}
	else
	{
		if (!IsIntegralType(lhs.type) ||
		    !IsIntegralOrUnscopedEnum(rhs.type))
			throw runtime_error("invalid compound assignment");
	}
	SemValue value;
	value.type = lhs.type;
	value.category = VC_LVALUE;
	value.node = MakeSemNode(SN_ASSIGNMENT_EXPRESSION);
	value.node->type = value.type;
	value.node->category = VC_LVALUE;
	value.node->has_op = true;
	value.node->op = expr.op;
	value.node->op_spelling = expr.op_spelling;
	value.node->children.push_back(std::move(lhs.node));
	value.node->children.push_back(std::move(rhs.node));
	return value;
}

// The 5.16p6/5.9p2 composite pointer type of two pointer operands, or
// null when they are incompatible. Cv-qualifiers merge at the first
// level; a void pointee absorbs an object pointee.
TypePtr SemExprAnalyzer::CompositePointerType(const SemValue& a,
                                              const SemValue& b)
{
	TypePtr pa = DecayToPointer(a.type);
	TypePtr pb = DecayToPointer(b.type);
	if (pa->kind != TK_POINTER || pb->kind != TK_POINTER)
		return TypePtr();
	const TypePtr& ta = pa->target;
	const TypePtr& tb = pb->target;
	bool merged_const = ta->is_const || tb->is_const;
	bool merged_volatile = ta->is_volatile || tb->is_volatile;
	TypePtr pointee;
	if (TypeEquals(RemoveTopCv(ta), RemoveTopCv(tb)))
		pointee = RemoveTopCv(ta);
	else if (IsVoidType(RemoveTopCv(ta)) && tb->kind != TK_FUNCTION)
		pointee = MakeFundamentalType(FT_VOID);
	else if (IsVoidType(RemoveTopCv(tb)) && ta->kind != TK_FUNCTION)
		pointee = MakeFundamentalType(FT_VOID);
	else if (ta->kind == TK_CLASS && tb->kind == TK_CLASS &&
	         BaseClassDistance(ta->named, tb->named) > 0)
		pointee = RemoveTopCv(tb);
	else if (ta->kind == TK_CLASS && tb->kind == TK_CLASS &&
	         BaseClassDistance(tb->named, ta->named) > 0)
		pointee = RemoveTopCv(ta);
	else
		return TypePtr();
	pointee = MakeCvQualifiedType(pointee, merged_const, merged_volatile);
	return MakePointerType(pointee, false, false);
}

TypePtr SemExprAnalyzer::ConditionalResultType(const SemValue& a,
                                               const SemValue& b,
                                               EValueCategory& category)
{
	category = VC_PRVALUE;
	// 5.16p4: same-typed glvalues of the same category stay glvalues.
	if (a.category == VC_LVALUE && b.category == VC_LVALUE &&
	    !a.function_set && !b.function_set &&
	    TypeEquals(a.type, b.type))
	{
		category = VC_LVALUE;
		return a.type;
	}
	if (a.type->kind == TK_CLASS && b.type->kind == TK_CLASS &&
	    a.category == VC_LVALUE && b.category == VC_LVALUE &&
	    !TypeEquals(RemoveTopCv(a.type), RemoveTopCv(b.type)))
	{
		// One operand converts to the other's (base) type; the result
		// stays an lvalue of the base.
		if (BaseClassDistance(a.type->named, b.type->named) > 0)
		{
			category = VC_LVALUE;
			return b.type;
		}
		if (BaseClassDistance(b.type->named, a.type->named) > 0)
		{
			category = VC_LVALUE;
			return a.type;
		}
	}
	// PA26 5.16p6: a member pointer arm composes with the other arm's
	// member pointer of the same type or a null pointer constant.
	bool a_pm = RemoveTopCv(a.type)->kind == TK_MEMBER_POINTER;
	bool b_pm = RemoveTopCv(b.type)->kind == TK_MEMBER_POINTER;
	if (a_pm || b_pm)
	{
		bool a_null = a.null_pointer_literal || IsNullPtrType(a.type);
		bool b_null = b.null_pointer_literal || IsNullPtrType(b.type);
		if (a_pm && (b_pm ? TypeEquals(RemoveTopCv(a.type),
		                               RemoveTopCv(b.type))
		                  : b_null))
			return RemoveTopCv(a.type);
		if (b_pm && a_null)
			return RemoveTopCv(b.type);
		throw runtime_error("incompatible conditional member pointer "
		                    "operands");
	}
	bool a_pointer = IsPointerAfterDecay(a.type) || IsNullPtrType(a.type) ||
		a.null_pointer_literal;
	bool b_pointer = IsPointerAfterDecay(b.type) || IsNullPtrType(b.type) ||
		b.null_pointer_literal;
	if (a_pointer && b_pointer)
	{
		bool a_null = a.null_pointer_literal || IsNullPtrType(a.type);
		bool b_null = b.null_pointer_literal || IsNullPtrType(b.type);
		if (a_null && IsPointerAfterDecay(b.type))
			return DecayToPointer(b.type);
		if (b_null && IsPointerAfterDecay(a.type))
			return DecayToPointer(a.type);
		if (a_null && b_null)
			return IsNullPtrType(a.type) || IsNullPtrType(b.type)
				? MakeFundamentalType(FT_NULLPTR_T)
				: MakeFundamentalType(FT_INT);
		if (TypePtr composite = CompositePointerType(a, b))
			return composite;
		throw runtime_error("incompatible conditional pointer operands");
	}
	if (a.type->kind == TK_ENUM && TypeEquals(RemoveTopCv(a.type),
	                                          RemoveTopCv(b.type)))
		return RemoveTopCv(a.type);
	if (IsVoidType(a.type) && IsVoidType(b.type))
		return MakeFundamentalType(FT_VOID);
	// 5.16p6: same-typed prvalue operands keep that type; the usual
	// arithmetic conversions only reconcile differing operand types.
	if (TypeEquals(RemoveTopCv(a.type), RemoveTopCv(b.type)))
		return RemoveTopCv(a.type);
	return UsualArithmeticConversions(a.type, b.type);
}

SemValue SemExprAnalyzer::AnalyzeConditional(const AstExpr& expr)
{
	SemValue cond = Analyze(*expr.operands[0]);
	TypePtr cond_class = RemoveTopCv(
		IsReferenceType(cond.type) ? cond.type->target : cond.type);
	RequireContextualBool(cond, "conditional condition");
	SemValue a = Analyze(*expr.operands[1]);
	SemValue b = Analyze(*expr.operands[2]);
	// An integral_constant-style condition (a class whose contextual
	// bool is a known constant) folds to the selected literal branch,
	// the reference presentation for trait-driven conditionals.
	bool known = false;
	if (cond_class->kind == TK_CLASS &&
	    host_.TryConstantClassBool(cond_class, known))
	{
		SemValue& pick = known ? a : b;
		SemValue& drop = known ? b : a;
		// PA26: member pointer constants (`&C::m`) fold like literal
		// arms - the non-selected branch materializes nothing.
		struct Foldable
		{
			static bool Arm(const SemValue& value)
			{
				if (!value.node)
					return false;
				if (value.node->has_value)
					return true;
				return value.node->kind == SN_UNARY_EXPRESSION &&
					value.node->has_op && value.node->op == OP_AMP &&
					value.type &&
					RemoveTopCv(value.type)->kind ==
						TK_MEMBER_POINTER;
			}
		};
		if (Foldable::Arm(pick) && Foldable::Arm(drop))
			return std::move(pick);
	}
	// 5.16p3: with one class-typed operand, the other operand converts
	// to the class type when an implicit conversion sequence exists
	// (the converted arm constructs the materialized result).
	{
		TypePtr a_bare = RemoveTopCv(a.type);
		TypePtr b_bare = RemoveTopCv(b.type);
		if ((a_bare->kind == TK_CLASS) != (b_bare->kind == TK_CLASS) &&
		    !a.function_set && !b.function_set)
		{
			SemValue& other = a_bare->kind == TK_CLASS ? b : a;
			TypePtr target = a_bare->kind == TK_CLASS ? a_bare : b_bare;
			ImplicitConversion conv = ClassifyConversion(
				MakeConversionSource(other), target);
			if (conv.viable)
				ApplyConversion(other, conv, target);
		}
	}
	EValueCategory category = VC_PRVALUE;
	TypePtr type = ConditionalResultType(a, b, category);
	// 5.16p6: a class-typed prvalue result converts glvalue arms to
	// prvalues (the lvalue-to-rvalue conversion copy-initializes). The
	// arms lower in place into the materialized result, so a glvalue
	// arm of a nontrivially-copyable class must wrap in its copy
	// construction rather than lower as a raw object copy (which would
	// alias the source's resources). Trivially copyable classes keep
	// the pinned raw-copy arm shape.
	if (category == VC_PRVALUE && RemoveTopCv(type)->kind == TK_CLASS)
	{
		const ClassInfo* cls =
			host_.Classes().Find(RemoveTopCv(type)->named);
		if (cls && !ClassHasTrivialCopyCtor(*cls))
		{
			if (a.category != VC_PRVALUE)
				WrapClassValueInit(a, RemoveTopCv(type));
			if (b.category != VC_PRVALUE)
				WrapClassValueInit(b, RemoveTopCv(type));
		}
	}
	SemValue value;
	value.type = type;
	value.category = category;
	value.node = MakeSemNode(SN_CONDITIONAL_EXPRESSION);
	value.node->type = type;
	value.node->category = category;
	if (category == VC_PRVALUE && RemoveTopCv(type)->kind == TK_CLASS)
	{
		// 12.2: a materialized class conditional result is a
		// destructible temporary of the full expression.
		const ClassInfo* cls =
			host_.Classes().Find(RemoveTopCv(type)->named);
		if (cls && host_.Classes().NeedsDestruction(*cls))
		{
			value.node->needs_dtor = true;
			value.node->result_dtor = host_.MakeTemporaryDtor(*cls);
		}
	}
	value.node->children.push_back(std::move(cond.node));
	value.node->children.push_back(std::move(a.node));
	value.node->children.push_back(std::move(b.node));
	return value;
}

SemValue SemExprAnalyzer::AnalyzeSubscript(const AstExpr& expr)
{
	SemValue first = Analyze(*expr.operands[0]);
	SemValue second = Analyze(*expr.operands[1]);
	if (first.type->kind == TK_CLASS)
	{
		// 13.5.5: operator[] is a member function; a class operand may
		// also reach the built-in form through a conversion function.
		vector<SemValue> operands;
		operands.push_back(std::move(first));
		operands.push_back(std::move(second));
		SemValue overloaded;
		if (ResolveOperatorCall("[]", operands, true, overloaded))
			return overloaded;
		first = std::move(operands[0]);
		second = std::move(operands[1]);
		if (!ConvertClassOperand(first))
			throw runtime_error("no operator[] for the class operand");
	}
	if (second.type->kind == TK_CLASS)
		ConvertClassOperand(second);
	// 5.2.1: one operand is the array/pointer, the other the index; the
	// dump normalizes the commuted form to array-first order.
	bool first_is_pointer = IsPointerAfterDecay(first.type);
	bool second_is_pointer = IsPointerAfterDecay(second.type);
	if (first_is_pointer == second_is_pointer)
		throw runtime_error("invalid subscript operands");
	SemValue& pointer = first_is_pointer ? first : second;
	SemValue& index = first_is_pointer ? second : first;
	if (!IsIntegralOrUnscopedEnum(index.type))
		throw runtime_error("subscript index is not integral");
	TypePtr element = pointer.type->target;
	if (pointer.type->kind == TK_POINTER &&
	    !IsObjectPointer(pointer.type))
		throw runtime_error("subscript of a non-object pointer");
	SemValue value;
	value.type = element;
	value.category = VC_LVALUE;
	value.node = MakeSemNode(SN_SUBSCRIPT_EXPRESSION);
	value.node->type = element;
	value.node->category = VC_LVALUE;
	value.node->children.push_back(std::move(pointer.node));
	value.node->children.push_back(std::move(index.node));
	return value;
}

SemValue SemExprAnalyzer::AnalyzeMember(const AstExpr& expr)
{
	SemValue object = Analyze(*expr.operands[0]);
	if (expr.op == OP_ARROW)
		object = DereferenceObject(std::move(object));
	if (!expr.name.IsPlainIdentifier())
		throw OutsideBoundary("member name form");
	return AnalyzeMemberAccess(std::move(object),
	                           expr.name.parts[0].identifier, expr.op,
	                           false);
}

// --- casts and sizeof -------------------------------------------------------

// Analyzes one argument/initializer list, expanding `pattern...` pack
// items in place (14.5.3). `allow_braced` admits braced-init-list
// arguments as deferred list-initialization values (8.5.4): their
// elements analyze now, the target initialization builds when overload
// resolution has selected a parameter.
void SemExprAnalyzer::AnalyzeArgumentList(const vector<AstExprPtr>& items,
                                          vector<SemValue>& out,
                                          bool allow_braced, size_t from)
{
	for (size_t i = from; i < items.size(); i++)
	{
		if (items[i]->kind == EK_PACK_EXPANSION)
		{
			if (!host_.ExpandPackExpression(*items[i]->operands[0], out))
				throw runtime_error("pack expansion outside an "
				                    "expandable context");
			continue;
		}
		if (allow_braced && items[i]->kind == EK_BRACED)
		{
			SemValue value;
			value.braced_list = true;
			AnalyzeArgumentList(items[i]->arguments, value.list_values);
			out.push_back(std::move(value));
			continue;
		}
		// PA34: a designated braced element (`.m = {...}`) defers like
		// a plain braced element, keeping its designator.
		if (allow_braced && items[i]->kind == EK_DESIGNATED &&
		    items[i]->operands[0]->kind == EK_BRACED)
		{
			SemValue value;
			value.braced_list = true;
			value.designator = items[i]->name.parts[0].identifier;
			AnalyzeArgumentList(items[i]->operands[0]->arguments,
			                    value.list_values);
			out.push_back(std::move(value));
			continue;
		}
		out.push_back(Analyze(*items[i]));
	}
}
