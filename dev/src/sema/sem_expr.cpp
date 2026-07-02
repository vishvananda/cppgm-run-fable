#include "sema/sem_expr.h"

#include <stdexcept>

#include "ast/ast_text.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA12 assignment boundary");
}

const AstExpr* StripParens(const AstExpr* expr)
{
	while (expr->kind == EK_PAREN)
		expr = expr->operands[0].get();
	return expr;
}

bool IsArithmeticOrEnum(const TypePtr& type)
{
	return IsArithmeticType(type) || type->kind == TK_ENUM;
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
	case EK_CSTYLE_CAST:
		return AnalyzeCastTo(host_.ResolveCastTypeId(*expr.type),
		                     *expr.operands[0], true, OP_LPAREN, "");
	case EK_KEYWORD_CAST:
		if (expr.op != KW_STATIC_CAST && expr.op != KW_CONST_CAST &&
		    expr.op != KW_REINTERPET_CAST)
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
	case EK_NEW:
		return AnalyzeNew(expr);
	case EK_DELETE:
		return AnalyzeDelete(expr);
	default:
		throw OutsideBoundary("expression form");
	}
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
		// non-static member function.
		TypePtr this_type = host_.CurrentThisType();
		if (!this_type)
			throw runtime_error("this outside a member function");
		value.type = this_type;
		value.node->kind = SN_ID_EXPRESSION;
		value.node->name = "this";
		value.node->token.clear();
		value.node->entity_scope = host_.CurrentScope();
		for (const Scope* scope = host_.CurrentScope(); scope;
		     scope = scope->parent)
			if (FindOwnBinding(*scope, "this"))
			{
				value.node->entity_scope = scope;
				break;
			}
		value.node->entity_name = "this";
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

SemValue SemExprAnalyzer::AnalyzeId(const AstExpr& expr)
{
	const NamedTypeInfo* member_class = 0;
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
	if (member_class && binding->kind == SB_VARIABLE &&
	    host_.CurrentThisType() &&
	    BaseClassDistance(host_.CurrentThisType()->target->named,
	                      member_class) >= 0)
	{
		const ClassInfo* owner_cls = 0;
		if (const NamedTypeInfo* owner_entity =
		        host_.Model().ScopeEntity(binding->owner))
			owner_cls = host_.Classes().Find(owner_entity);
		if (owner_cls && FindClassField(*owner_cls, binding->name))
			return AnalyzeImplicitMember(*binding, written);
	}
	SemValue value;
	switch (binding->kind)
	{
	case SB_VARIABLE:
	case SB_PARAMETER:
	{
		// A constant static member named through a qualified-id folds
		// like an enumerator (9.4.2p4 constant initializer).
		if (member_class && binding->has_value)
			return AnalyzeStaticMemberValue(*binding, written);
		// 5.3.1p3: only non-static data members carry the member
		// -pointer facts; a static member behaves as an ordinary
		// namespace-scope object.
		if (member_class)
		{
			const ClassInfo* owner_cls = 0;
			if (const NamedTypeInfo* owner_entity =
			        host_.Model().ScopeEntity(binding->owner))
				owner_cls = host_.Classes().Find(owner_entity);
			if (!owner_cls || !FindClassField(*owner_cls, binding->name))
				member_class = 0;
		}
		// 5p5: the expression type is the declared type with references
		// stripped; the result is an lvalue either way.
		TypePtr declared = binding->type;
		if (IsReferenceType(declared))
			declared = declared->target;
		value.type = declared;
		value.category = VC_LVALUE;
		value.member_class = member_class;
		value.member_type = declared;
		if (!binding->anon_storage_name.empty())
		{
			// Injected anonymous-union member (9.5p5): a synthesized
			// access through the storage variable.
			SemNodePtr storage = MakeSemNode(SN_ID_EXPRESSION);
			storage->name = binding->anon_storage_name;
			storage->type = binding->anon_storage_type;
			storage->category = VC_LVALUE;
			value.node = MakeSemNode(SN_MEMBER_EXPRESSION);
			value.node->name = written;
			value.node->type = declared;
			value.node->category = VC_LVALUE;
			value.node->children.push_back(std::move(storage));
			return value;
		}
		break;
	}
	case SB_FUNCTION:
	{
		value.function_set = true;
		if (binding->type)
		{
			value.overloads.push_back(binding->type);
			for (size_t i = 0; i < binding->overloads.size(); i++)
				value.overloads.push_back(binding->overloads[i]);
		}
		value.fn_templates = binding->fn_templates;
		value.overload_specs.resize(value.overloads.size(), 0);
		value.fn_owner = binding->owner;
		value.fn_name = binding->name;
		value.category = VC_LVALUE;
		value.member_class = member_class;
		value.member_type = binding->type;
		value.type = member_class
			? ThisAdjustedType(member_class, binding->type)
			: binding->type;
		break;
	}
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
	value.node = MakeSemNode(SN_ID_EXPRESSION);
	value.node->name = written;
	value.node->type = value.type;
	value.node->category = value.category;
	value.node->entity_scope = binding->owner;
	value.node->entity_name = binding->name;
	return value;
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
		if (const ClassInfo* cls =
		        host_.Classes().Find(RemoveTopCv(result)->named))
			if (host_.Classes().NeedsDestruction(*cls))
			{
				value.node->needs_dtor = true;
				value.node->result_dtor = host_.MakeTemporaryDtor(*cls);
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

// 12.3.1: the converting constructor builds a temporary of the
// destination class from the (standard-converted) source.
void SemExprAnalyzer::ApplyConstructorConversion(
	SemValue& value, const ImplicitConversion& conv)
{
	const ClassInfo* cls = host_.Classes().Find(conv.user_class);
	if (!cls)
		throw runtime_error("converting constructor class record "
		                    "missing");
	const TypePtr& param = cls->ctors[conv.user_ctor].type->parameters[0];
	ImplicitConversion inner =
		ClassifyConversion(MakeConversionSource(value), param);
	ApplyConversion(value, inner, param);
	vector<SemNodePtr> args;
	args.push_back(std::move(value.node));
	SemNodePtr action = host_.MakeConstructorCall(
		*cls, conv.user_ctor, false, SemNodePtr(), std::move(args));
	action->category = VC_PRVALUE;
	if (host_.Classes().NeedsDestruction(*cls))
	{
		action->needs_dtor = true;
		action->children.push_back(host_.MakeTemporaryDtor(*cls));
	}
	TypePtr class_type = MakeNamedType(TK_CLASS, conv.user_class);
	action->type = class_type;
	value.node = std::move(action);
	value.type = class_type;
	value.category = VC_PRVALUE;
	value.null_pointer_literal = false;
}

void SemExprAnalyzer::ApplyConversion(SemValue& value,
                                      const ImplicitConversion& conv,
                                      const TypePtr& dest)
{
	if (conv.user_ctor >= 0 && conv.user_class)
	{
		ApplyConstructorConversion(value, conv);
		return;
	}
	if (conv.conv_index >= 0 && conv.conv_class)
	{
		// 12.3.2: the conversion function call produces the converted
		// value; any remaining standard conversion happens in context.
		const ClassInfo* cls = host_.Classes().Find(conv.conv_class);
		if (!cls)
			throw runtime_error("conversion function class record "
			                    "missing");
		const ClassConversion& fn = cls->conversions[conv.conv_index];
		host_.CheckMemberAccess(cls->members, fn.access, fn.name);
		int hops = BaseClassDistance(RemoveTopCv(value.type)->named,
		                             conv.conv_class);
		if (hops > 0)
		{
			SemNodePtr adjusted = MakeSemNode(SN_MEMBER_EXPRESSION);
			adjusted->type = MakeNamedType(TK_CLASS, conv.conv_class);
			adjusted->category = VC_LVALUE;
			adjusted->base_hops = hops;
			adjusted->children.push_back(std::move(value.node));
			value.node = std::move(adjusted);
		}
		SemValue out = CallResult(fn.type);
		SemNodePtr callee = MakeSemNode(SN_CALLEE);
		callee->name = CanonicalQualifiedName(cls->members, fn.name);
		callee->type = ThisAdjustedType(conv.conv_class, fn.type);
		callee->entity_scope = cls->members;
		callee->entity_name = fn.name;
		callee->is_method = true;
		if (const ScopeBinding* binding =
		        FindOwnBinding(*cls->members, fn.name))
		{
			size_t index = 0;
			for (size_t i = 0; i < binding->overloads.size(); i++)
				if (TypeEquals(binding->overloads[i], fn.type))
					index = i + 1;
			if (index < binding->fn_unwind_no.size() &&
			    binding->fn_unwind_no[index])
				callee->unwind_no = true;
		}
		out.node->children.push_back(std::move(callee));
		out.node->children.push_back(
			AddressOfObject(std::move(value.node)));
		bool null_literal = value.null_pointer_literal;
		(void)null_literal;
		value = std::move(out);
		return;
	}
	if (conv.selected_overload >= 0)
	{
		// 13.4: the target type picked one overload; the id-expression
		// node now shows the selected function's type.
		value.type = value.overloads[conv.selected_overload];
		if (!value.node)
		{
			// A member-access function set carries no expression node;
			// the selected (static) member synthesizes its
			// id-expression here.
			value.node = MakeSemNode(SN_ID_EXPRESSION);
			value.node->category = value.category;
			if (value.member_fn)
			{
				value.node->name = value.member_fn->name;
				value.node->entity_scope = value.member_fn->owner;
				value.node->entity_name = value.member_fn->name;
			}
		}
		value.node->type = value.type;
		// A deduced specialization re-targets the node's identity.
		if ((size_t)conv.selected_overload < value.overload_specs.size())
			if (const FunctionSpecialization* spec =
			        value.overload_specs[conv.selected_overload])
			{
				value.node->entity_scope = spec->self.owner;
				value.node->entity_name = spec->self.name;
				value.node->fn_spec = spec;
				value.fn_owner = spec->self.owner;
				value.fn_name = spec->self.name;
			}
		value.function_set = false;
		value.overloads.clear();
		value.overload_specs.clear();
	}
	if (conv.null_to_pointer)
	{
		// The dump retypes a converted null pointer literal in place.
		TypePtr target = IsReferenceType(dest)
			? RemoveTopCv(dest->target) : RemoveTopCv(dest);
		value.type = target;
		value.node->type = target;
		value.node->null_pointer = true;
		value.null_pointer_literal = false;
	}
	if (!IsReferenceType(dest))
	{
		// PA16: a by-value class destination copy/move-initializes from
		// a class source unless the source already constructs the
		// destination object in place.
		TypePtr bare = RemoveTopCv(dest);
		if (bare->kind == TK_CLASS && value.type && !value.function_set &&
		    RemoveTopCv(value.type)->kind == TK_CLASS)
		{
			// A same-class prvalue (temporary, call result) constructs
			// the destination in place (12.8p31). A conditional does
			// so only for a trivially copyable class (its arms lower
			// as raw object copies); otherwise it materializes its own
			// temporary and the destination copy/move-constructs.
			bool in_place = value.node &&
				value.category == VC_PRVALUE &&
				RemoveTopCv(value.type)->named == bare->named;
			if (in_place &&
			    value.node->kind == SN_CONDITIONAL_EXPRESSION)
			{
				const ClassInfo* cls =
					host_.Classes().Find(bare->named);
				in_place = cls && ClassHasTrivialCopyCtor(*cls);
			}
			if (!in_place)
				WrapClassValueInit(value, bare);
		}
	}
}

void SemExprAnalyzer::WrapClassValueInit(SemValue& value, const TypePtr& bare)
{
	const ClassInfo* cls = host_.Classes().Find(bare->named);
	if (!cls || !bare->named->complete)
		throw runtime_error("copy of an incomplete class object");
	vector<SemValue> args;
	args.push_back(std::move(value));
	int index = host_.ResolveClassCtorHost(*cls, args, true,
	                                       "initialization");
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < args.size(); i++)
		arg_nodes.push_back(std::move(args[i].node));
	SemNodePtr action = host_.MakeConstructorCall(
		*cls, index, false, SemNodePtr(), std::move(arg_nodes));
	action->synth_copy = true;
	action->type = RemoveTopCv(bare);
	action->category = VC_PRVALUE;
	if (host_.Classes().NeedsDestruction(*cls))
	{
		action->needs_dtor = true;
		action->children.push_back(host_.MakeTemporaryDtor(*cls));
	}
	value = SemValue();
	value.type = action->type;
	value.category = VC_PRVALUE;
	value.node = std::move(action);
}

void SemExprAnalyzer::CopyInitialize(SemValue& value, const TypePtr& dest,
                                     const char* what)
{
	AddTargetDeducedOverloads(value, dest);
	ImplicitConversion conv =
		ClassifyConversion(MakeConversionSource(value), dest);
	if (!conv.viable)
		throw runtime_error(string("no conversion for ") + what);
	ApplyConversion(value, conv, dest);
}

void SemExprAnalyzer::RequireContextualBool(SemValue& value,
                                            const char* what)
{
	// 4p4: contextual bool conversion; explicit conversion functions
	// participate (direct-initialization semantics).
	ImplicitConversion conv = ClassifyConversionEx(
		MakeConversionSource(value), MakeFundamentalType(FT_BOOL), true);
	if (!conv.viable)
		throw runtime_error(string(what) + " is not contextually bool");
	if (conv.conv_index >= 0 && value.node)
		ApplyConversion(value, conv, MakeFundamentalType(FT_BOOL));
}

// A class operand of a built-in operator form converts through its
// single viable non-explicit conversion function (the 13.6 subset).
bool SemExprAnalyzer::ConvertClassOperand(SemValue& value)
{
	TypePtr bare = RemoveTopCv(value.type);
	if (bare->kind != TK_CLASS || !bare->named->class_record)
		return false;
	bool source_const = false;
	bool source_volatile = false;
	TopCv(value.type, source_const, source_volatile);
	const ClassConversion* found = 0;
	int found_score = 3;
	for (const ClassInfo* link = bare->named->class_record; link;
	     link = link->base)
		for (size_t i = 0; i < link->conversions.size(); i++)
		{
			const ClassConversion& conv = link->conversions[i];
			if (conv.is_explicit)
				continue;
			// The implicit object binding selects among cv-qualified
			// overloads: a const source requires a const function.
			if (source_const && !conv.type->is_const)
				continue;
			int score = conv.type->is_const == source_const ? 0 : 1;
			if (found && score == found_score &&
			    !TypeEquals(found->result, conv.result))
				return false;  // no unique built-in operand form
			if (!found || score < found_score)
			{
				found = &conv;
				found_score = score;
			}
		}
	if (!found)
		return false;
	TypePtr dest = IsReferenceType(found->result)
		? found->result->target : RemoveTopCv(found->result);
	ImplicitConversion conv = ClassifyConversionEx(
		MakeConversionSource(value), dest, false);
	if (!conv.viable || conv.conv_index < 0)
		return false;
	ApplyConversion(value, conv, dest);
	return true;
}

void SemExprAnalyzer::RequireModifiableLvalue(const SemValue& value,
                                              const char* what)
{
	if (value.category != VC_LVALUE)
		throw runtime_error(string(what) + " requires an lvalue");
	bool is_const = false;
	bool is_volatile = false;
	TopCv(value.type, is_const, is_volatile);
	if (is_const || value.type->kind == TK_ARRAY ||
	    value.type->kind == TK_FUNCTION || value.function_set)
		throw runtime_error(string(what) + " requires a modifiable lvalue");
}

// --- calls ---------------------------------------------------------------

SemValue SemExprAnalyzer::AnalyzeCall(const AstExpr& expr)
{
	const AstExpr* callee = StripParens(expr.operands[0].get());
	if (callee->kind == EK_MEMBER)
		return AnalyzeMemberCall(expr, *callee);
	if (callee->kind == EK_ID)
	{
		// 3.4.2p1: a parenthesized callee suppresses argument-dependent
		// lookup.
		bool paren_callee = expr.operands[0]->kind == EK_PAREN;
		bool plain = callee->name.IsPlainIdentifier();
		if (TypePtr as_type = host_.TryResolveCalleeType(callee->name))
			return AnalyzeFunctionalCast(as_type, expr.arguments);
		const NamedTypeInfo* member_class = 0;
		const ScopeBinding* binding = 0;
		try
		{
			binding = host_.ResolveValue(callee->name, member_class);
		}
		catch (const AmbiguousLookupError&)
		{
			// 7.3.4p6: same-level function declarations form one
			// overload set rather than an ambiguity; any other
			// ambiguous lookup is ill-formed regardless of what a
			// builtin or ADL retry could supply (3.4.1p2).
			if (!plain || paren_callee)
				throw;
			vector<const ScopeBinding*> fn_set;
			UnqualifiedLookup(host_.CurrentScope(),
			                  callee->name.parts[0].identifier,
			                  SLF_ANY, &fn_set);
			return AnalyzeAdlCall(
				expr, callee->name.parts[0].identifier, fn_set);
		}
		catch (const std::exception&)
		{
			if (plain &&
			    callee->name.parts[0].identifier == "__builtin_constant_p")
				return AnalyzeBuiltinConstantP(expr);
			if (plain)
				binding = host_.ResolveBuiltinFunction(
					callee->name.parts[0].identifier);
			if (!binding && plain && !paren_callee)
				// Only argument-dependent lookup can name the callee
				// (hidden friends, associated namespaces).
				return AnalyzeAdlCall(
					expr, callee->name.parts[0].identifier,
					vector<const ScopeBinding*>());
			if (!binding)
				throw;
		}
		if (binding->kind == SB_FUNCTION)
		{
			if (plain && !paren_callee && binding->home &&
			    binding->home->kind == SCOPE_NAMESPACE)
				return AnalyzeAdlCall(
					expr, callee->name.parts[0].identifier,
					vector<const ScopeBinding*>(1, binding));
			// 10.3p15: explicit scope qualification suppresses the
			// virtual call mechanism.
			bool qualified = callee->name.parts.size() > 1 ||
				callee->name.global_scope;
			return AnalyzeNamedCall(expr, *binding, member_class,
			                        qualified);
		}
	}
	return AnalyzeIndirectCall(expr);
}

SemValue SemExprAnalyzer::AnalyzeNamedCall(const AstExpr& expr,
                                           const ScopeBinding& binding,
                                           const NamedTypeInfo* member_class,
                                           bool qualified)
{
	// A member function named without an object: bind the implicit
	// *this when the context provides one; otherwise only static
	// members are callable (9.3.1p3).
	const NamedTypeInfo* home_entity = 0;
	if (binding.home && binding.home->kind == SCOPE_CLASS)
		home_entity = host_.Model().ScopeEntity(binding.home);
	if (member_class || home_entity)
	{
		const NamedTypeInfo* target = member_class ? member_class
		                                           : home_entity;
		TypePtr this_type = host_.CurrentThisType();
		if (this_type &&
		    BaseClassDistance(this_type->target->named, target) >= 0)
		{
			SemValue object;
			object.node = ImplicitThisObject();
			object.type = object.node->type;
			object.category = VC_LVALUE;
			return AnalyzeMethodCall(std::move(object), binding,
			                         expr.arguments, qualified);
		}
		return AnalyzeStaticMethodCall(expr, binding);
	}
	vector<SemValue> args;
	vector<ConversionSource> sources;
	for (size_t i = 0; i < expr.arguments.size(); i++)
	{
		args.push_back(Analyze(*expr.arguments[i]));
		sources.push_back(MakeConversionSource(args.back()));
	}
	vector<TypePtr> candidates;
	if (binding.type)
	{
		candidates.push_back(binding.type);
		for (size_t i = 0; i < binding.overloads.size(); i++)
			candidates.push_back(binding.overloads[i]);
	}
	// PA18: deduced function-template specializations join after the
	// ordinary overloads.
	// An explicit template-id callee pre-binds the leading template
	// parameters (14.8.1).
	const AstNamePart* explicit_part = 0;
	{
		const AstExpr* callee = StripParens(expr.operands[0].get());
		if (callee->kind == EK_ID && !callee->name.parts.empty() &&
		    callee->name.parts.back().kind == NP_TEMPLATE_ID)
			explicit_part = &callee->name.parts.back();
	}
	const size_t ordinary = candidates.size();
	vector<const FunctionSpecialization*> specs(ordinary, (const FunctionSpecialization*)0);
	{
		std::set<const void*> seen;
		vector<OperatorCandidate> deduced;
		AppendTemplateCandidates(binding, args, deduced, seen,
		                         explicit_part);
		for (size_t i = 0; i < deduced.size(); i++)
		{
			candidates.push_back(deduced[i].declared);
			specs.push_back(deduced[i].spec);
		}
	}
	// PA18 13.4: overloaded/template arguments deduce against every
	// candidate's parameter types before ranking.
	bool augmented = false;
	for (size_t c = 0; c < candidates.size(); c++)
		for (size_t i = 0;
		     i < args.size() && i < candidates[c]->parameters.size(); i++)
			if (args[i].function_set && !args[i].fn_templates.empty())
			{
				AddTargetDeducedOverloads(
					args[i], candidates[c]->parameters[i]);
				augmented = true;
			}
	if (augmented)
	{
		sources.clear();
		for (size_t i = 0; i < args.size(); i++)
			sources.push_back(MakeConversionSource(args[i]));
	}
	// 8.3.6: trailing default arguments lower each candidate's minimum
	// call arity.
	vector<size_t> min_arity(candidates.size());
	vector<bool> is_template(candidates.size(), false);
	for (size_t c = 0; c < candidates.size(); c++)
	{
		size_t required = candidates[c]->parameters.size();
		const vector<const AstExpr*>* defaults = 0;
		if (c < ordinary)
			defaults = c < binding.fn_defaults.size()
				? &binding.fn_defaults[c] : 0;
		else
		{
			defaults = &specs[c]->self.fn_defaults[0];
			is_template[c] = true;
		}
		while (defaults && required > 0 &&
		       required <= defaults->size() && (*defaults)[required - 1])
			required--;
		min_arity[c] = required;
	}
	vector<ImplicitConversion> conversions;
	size_t winner = SelectBestOverload(candidates, sources, conversions,
	                                   &min_arity, &is_template);
	const FunctionSpecialization* spec =
		winner < ordinary ? 0 : specs[winner];
	const ScopeBinding& chosen = spec ? spec->self : binding;
	const size_t slot = spec ? 0 : winner;
	if (slot < chosen.fn_deleted.size() && chosen.fn_deleted[slot])
		throw runtime_error("use of deleted function " + binding.name);
	const TypePtr& function_type = candidates[winner];
	for (size_t i = 0; i < args.size(); i++)
		if (i < function_type->parameters.size())
			ApplyConversion(args[i], conversions[i],
			                function_type->parameters[i]);
	// Synthesize the omitted trailing arguments from the recorded
	// default expressions (8.3.6p9: evaluated at the call site;
	// instantiated signatures look their names up in their argument
	// alias scope).
	for (size_t i = args.size(); i < function_type->parameters.size(); i++)
	{
		Scope* default_scope = TemplateDefaultArgScope(chosen);
		Scope* saved = default_scope
			? host_.SwapLookupScope(default_scope) : 0;
		SemValue filled;
		try
		{
			filled = Analyze(*chosen.fn_defaults[slot][i]);
		}
		catch (...)
		{
			if (default_scope)
				host_.SwapLookupScope(saved);
			throw;
		}
		if (default_scope)
			host_.SwapLookupScope(saved);
		CopyInitialize(filled, function_type->parameters[i],
		               "default argument");
		args.push_back(std::move(filled));
	}

	SemValue value = CallResult(function_type);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(chosen.owner, chosen.name);
	callee->type = function_type;
	callee->entity_scope = chosen.owner;
	callee->entity_name = chosen.name;
	callee->fn_spec = spec;
	if (slot < chosen.fn_unwind_no.size() && chosen.fn_unwind_no[slot])
		callee->unwind_no = true;
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

void SemExprAnalyzer::CheckCallArguments(const TypePtr& function_type,
                                         vector<SemValue>& args)
{
	const vector<TypePtr>& params = function_type->parameters;
	if (args.size() < params.size() ||
	    (args.size() > params.size() && !function_type->variadic))
		throw runtime_error("wrong number of call arguments");
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i < params.size())
			CopyInitialize(args[i], params[i], "call argument");
		else if (args[i].function_set && args[i].overloads.size() > 1)
			throw runtime_error("overloaded name as ellipsis argument");
	}
}

SemValue SemExprAnalyzer::AnalyzeIndirectCall(const AstExpr& expr)
{
	SemValue fn = Analyze(*expr.operands[0]);
	if (fn.type->kind == TK_CLASS)
		return AnalyzeFunctorCall(std::move(fn), expr);
	TypePtr function_type;
	if (fn.type->kind == TK_FUNCTION)
		function_type = fn.type;
	else if (fn.type->kind == TK_POINTER &&
	         fn.type->target->kind == TK_FUNCTION)
		function_type = fn.type->target;
	else
		throw runtime_error("called object is not a function");
	if (fn.function_set && fn.overloads.size() > 1)
		throw runtime_error("overloaded name in a non-call context");

	vector<SemValue> args;
	for (size_t i = 0; i < expr.arguments.size(); i++)
		args.push_back(Analyze(*expr.arguments[i]));
	CheckCallArguments(function_type, args);

	SemValue value = CallResult(function_type);
	value.node->children.push_back(std::move(fn.node));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

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
		// 5.3.1p3-p4: &C::member forms a member pointer over the
		// declared (cv-qualified) member type.
		value.type = MakeMemberPointerType(operand.member_class,
		                                   operand.member_type,
		                                   false, false);
	}
	else if (operand.function_set)
	{
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
	default:
		throw OutsideBoundary("binary operator");
	}
}

SemValue SemExprAnalyzer::AnalyzeAssignment(const AstExpr& expr)
{
	SemValue lhs = Analyze(*expr.operands[0]);
	const AstExpr& right = *expr.operands[1];
	SemValue rhs;
	if (right.kind == EK_BRACED && expr.op == OP_ASS &&
	    lhs.type->kind == TK_CLASS)
		// 5.17p9: a braced-init-list right operand list-initializes
		// a temporary of the left operand's class type.
		rhs = MakeTemporaryObject(RemoveTopCv(lhs.type),
		                          right.arguments, true);
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
	RequireContextualBool(cond, "conditional condition");
	SemValue a = Analyze(*expr.operands[1]);
	SemValue b = Analyze(*expr.operands[2]);
	EValueCategory category = VC_PRVALUE;
	TypePtr type = ConditionalResultType(a, b, category);
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

SemNodePtr SemExprAnalyzer::AnalyzeBracedInit(const AstExpr& braced,
                                              TypePtr& dest)
{
	if (dest->kind != TK_ARRAY)
		throw OutsideBoundary("braced initialization form");
	const vector<AstExprPtr>& elements = braced.arguments;
	if (dest->bound_known && elements.size() > dest->bound)
		throw runtime_error("too many braced initializers");
	if (!dest->bound_known)
		// 8.5.1p4: the array bound completes from the initializer list.
		dest = MakeArrayType(dest->target, true, elements.size());
	SemNodePtr node = MakeSemNode(SN_BRACED_INIT_LIST);
	node->type = dest;
	node->category = VC_LVALUE;
	for (size_t i = 0; i < elements.size(); i++)
	{
		SemValue element = Analyze(*elements[i]);
		CopyInitialize(element, dest->target, "array element");
		node->children.push_back(std::move(element.node));
	}
	return node;
}
