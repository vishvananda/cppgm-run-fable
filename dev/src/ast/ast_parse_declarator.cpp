#include "ast/ast_parser.h"

#include "hosted_probes.h"

using std::string;
using std::vector;
using std::move;

namespace {

// Specifier sequence states: at most one type per sequence (7.1.6/2);
// simple-type keywords may combine with each other (`unsigned long`),
// a named type closes the sequence to further types.
enum
{
	kNoType = 0,
	kKeywordType = 1,
	kNamedType = 2
};

bool IsSimpleTypeSpecifier(ETokenType type)
{
	switch (type)
	{
	case KW_BOOL: case KW_CHAR: case KW_CHAR16_T: case KW_CHAR32_T:
	case KW_DOUBLE: case KW_FLOAT: case KW_INT: case KW_LONG:
	case KW_SHORT: case KW_SIGNED: case KW_UNSIGNED: case KW_VOID:
	case KW_WCHAR_T: case KW_AUTO:
		return true;
	default:
		return false;
	}
}

bool IsDeclOnlySpecifier(ETokenType type)
{
	switch (type)
	{
	case KW_TYPEDEF: case KW_EXTERN: case KW_STATIC: case KW_INLINE:
	case KW_VIRTUAL: case KW_CONSTEXPR: case KW_THREAD_LOCAL:
	case KW_FRIEND: case KW_MUTABLE:
		return true;
	default:
		return false;
	}
}

}  // namespace

bool AstParser::ParseOneSpecifier(AstSpecifierSeq& seq, ESeqKind kind,
                                  int& type_state)
{
	SkipDeclAdornments();
	// PA34: [[...]] attributes in specifier positions (leading
	// attribute-specifier-seq of hosted declarations), discarded. The
	// enclosing sequence restores if no real specifier follows.
	while (SkipSquareAttribute())
		SkipDeclAdornments();
	const ParseToken& token = Peek();
	if (token.kind == PTOK_SIMPLE)
	{
		ETokenType type = token.simple_type;
		if (type == KW_CONST || type == KW_VOLATILE ||
		    (kind == kDeclSpecifierSeq && IsDeclOnlySpecifier(type)))
		{
			AstSpecifier spec;
			spec.kind = SPEC_KEYWORD;
			spec.keyword = type;
			spec.spelling = token.spelling;
			Advance();
			seq.push_back(move(spec));
			return true;
		}
		if (IsSimpleTypeSpecifier(type) && type_state != kNamedType)
		{
			AstSpecifier spec;
			spec.kind = SPEC_KEYWORD;
			spec.keyword = type;
			spec.spelling = token.spelling;
			Advance();
			seq.push_back(move(spec));
			type_state = kKeywordType;
			return true;
		}
		if (type == KW_DECLTYPE && type_state == kNoType)
		{
			State state = Save();
			Advance();
			AstExprPtr expr;
			if (MatchSimple(OP_LPAREN) && (expr = ParseExpression()) &&
			    MatchSimple(OP_RPAREN))
			{
				AstSpecifier spec;
				spec.kind = SPEC_DECLTYPE;
				spec.decltype_expr = move(expr);
				seq.push_back(move(spec));
				type_state = kNamedType;
				return true;
			}
			Restore(state);
			return false;
		}
		if ((type == KW_CLASS || type == KW_STRUCT || type == KW_UNION) &&
		    type_state == kNoType)
		{
			AstDeclPtr nested = ParseClassSpecifier();
			if (!nested)
				nested = ParseElaboratedClass();
			if (!nested)
				return false;
			AstSpecifier spec;
			spec.kind = SPEC_NESTED_DECL;
			spec.nested_decl = move(nested);
			seq.push_back(move(spec));
			type_state = kNamedType;
			return true;
		}
		if (type == KW_ENUM && type_state == kNoType)
		{
			AstDeclPtr nested = ParseEnumSpecifier();
			if (!nested)
				return false;
			AstSpecifier spec;
			spec.kind = SPEC_NESTED_DECL;
			spec.nested_decl = move(nested);
			seq.push_back(move(spec));
			type_state = kNamedType;
			return true;
		}
		if (type == KW_TYPENAME && type_state == kNoType)
		{
			State state = Save();
			AstSpecifier spec;
			spec.kind = SPEC_TYPE_NAME;
			if (!ParseQualifiedTypeName(spec.name))
			{
				Restore(state);
				return false;
			}
			seq.push_back(move(spec));
			type_state = kNamedType;
			return true;
		}
		if (type != OP_COLON2)
			return false;
	}
	// PA33 __decay / PA34 transform family: __remove_cv ( type-id ).
	// PA36: GCC 15 also exposes these as builtin alias templates, so
	// the template-id spelling __remove_reference_t < type-id > is the
	// same transform.
	if (type_state == kNoType && Peek().kind == PTOK_IDENTIFIER &&
	    HostedBuiltinTransformName(Peek().spelling) &&
	    (AtSimple(OP_LPAREN, 1) || AtSimple(OP_LT, 1)))
	{
		bool angle_form = AtSimple(OP_LT, 1);
		State state = Save();
		AstSpecifier spec;
		spec.kind = SPEC_TRANSFORM;
		spec.spelling = Peek().spelling;
		Advance();
		if (angle_form)
			MatchOpenAngle();
		else
			Advance();
		if (ParseTypeId(spec.transform_type) &&
		    (angle_form ? MatchCloseAngle() : MatchSimple(OP_RPAREN)))
		{
			seq.push_back(move(spec));
			type_state = kNamedType;
			return true;
		}
		Restore(state);
		return false;
	}
	// GNU __typeof__/typeof: decltype semantics (references
	// stripped) over an expression or type operand. C11 _Atomic(T)
	// rides the same shape: the atomic qualification is accepted and
	// dropped (the atomic builtin operators carry the semantics).
	if (type_state == kNoType && Peek().kind == PTOK_IDENTIFIER &&
	    (Peek().spelling == "__typeof__" ||
	     Peek().spelling == "__typeof" || Peek().spelling == "typeof" ||
	     Peek().spelling == "_Atomic") &&
	    AtSimple(OP_LPAREN, 1))
	{
		State state = Save();
		bool atomic = Peek().spelling == "_Atomic";
		Advance();
		Advance();
		AstSpecifier spec;
		spec.kind = SPEC_DECLTYPE;
		spec.typeof_strip = true;
		// _Atomic's operand is always a type; typeof reads its
		// expression form first.
		if (!atomic)
		{
			AstExprPtr expr = ParseExpression();
			if (expr && MatchSimple(OP_RPAREN))
			{
				spec.decltype_expr = move(expr);
				seq.push_back(move(spec));
				type_state = kNamedType;
				return true;
			}
			Restore(state);
			Advance();
			Advance();
		}
		if (ParseTypeId(spec.transform_type) && MatchSimple(OP_RPAREN))
		{
			seq.push_back(move(spec));
			type_state = kNamedType;
			return true;
		}
		Restore(state);
		return false;
	}
	// GNU _Complex/__complex__: the complex qualification records
	// beside the element keywords (`__complex__ long double`).
	if (token.kind == PTOK_IDENTIFIER &&
	    (token.spelling == "_Complex" ||
	     token.spelling == "__complex__") && type_state != kNamedType)
	{
		AstSpecifier spec;
		spec.kind = SPEC_COMPLEX;
		spec.spelling = token.spelling;
		Advance();
		seq.push_back(move(spec));
		return true;
	}
	// GNU __int128: a base-type specifier spelled as an identifier;
	// sign keywords may appear on either side (`__int128 unsigned`),
	// so it sets the keyword-combination state, not the named state.
	if (token.kind == PTOK_IDENTIFIER && token.spelling == "__int128" &&
	    type_state != kNamedType)
	{
		AstSpecifier spec;
		spec.kind = SPEC_TYPE_NAME;
		AstNamePart part;
		part.kind = NP_IDENTIFIER;
		part.identifier = token.spelling;
		spec.name.parts.push_back(move(part));
		Advance();
		seq.push_back(move(spec));
		type_state = kKeywordType;
		return true;
	}
	// Clang/C23 _BitInt ( constant-expression ): a bit-precise base
	// type; sign keywords combine like __int128.
	if (token.kind == PTOK_IDENTIFIER && token.spelling == "_BitInt" &&
	    type_state != kNamedType && AtSimple(OP_LPAREN, 1))
	{
		State state = Save();
		Advance();
		Advance();
		AstExprPtr width = ParseConditionalExpression();
		if (width && MatchSimple(OP_RPAREN))
		{
			AstSpecifier spec;
			spec.kind = SPEC_BITINT;
			spec.decltype_expr = move(width);
			seq.push_back(move(spec));
			type_state = kKeywordType;
			return true;
		}
		Restore(state);
		return false;
	}
	// A builtin trait name followed by ( is always the PA34 trait
	// expression, never an unknown type-name (`X<__is_function(T)>`
	// must not re-read as a functional cast).
	if (token.kind == PTOK_IDENTIFIER &&
	    HostedBuiltinTraitName(token.spelling) && AtSimple(OP_LPAREN, 1))
		return false;
	// A registered builtin function name is never a type-name (the
	// declaration reading of `__sync_lock_release(&lock);` must not
	// shadow the call statement).
	if (token.kind == PTOK_IDENTIFIER &&
	    HostedProbeHasBuiltin(token.spelling))
		return false;
	if ((token.kind == PTOK_IDENTIFIER || AtSimple(OP_COLON2)) &&
	    type_state == kNoType)
	{
		State state = Save();
		AstSpecifier spec;
		spec.kind = SPEC_TYPE_NAME;
		if (!ParseQualifiedTypeName(spec.name) ||
		    !NameUsableAsType(spec.name))
		{
			Restore(state);
			return false;
		}
		seq.push_back(move(spec));
		type_state = kNamedType;
		return true;
	}
	return false;
}

bool AstParser::ParseSpecifierSeq(AstSpecifierSeq& seq, ESeqKind kind)
{
	State state = Save();
	int type_state = kNoType;
	while (ParseOneSpecifier(seq, kind, type_state))
	{
	}
	if (seq.empty())
	{
		Restore(state);
		return false;
	}
	return true;
}

// type-id: type-specifier-seq abstract-declarator?
bool AstParser::ParseTypeId(AstTypeIdPtr& type)
{
	State state = Save();
	AstTypeIdPtr result(new AstTypeId());
	if (!ParseSpecifierSeq(result->specifiers, kTypeSpecifierSeq))
	{
		Restore(state);
		return false;
	}
	AstDeclaratorPtr declarator;
	if (!ParseDeclarator(declarator, false))
	{
		Restore(state);
		return false;
	}
	if (declarator && !declarator->Empty())
		result->declarator = move(declarator);
	type = move(result);
	return true;
}

// ptr-operator*: * & && and member pointers NNS::*, each pointer
// optionally followed by cv-qualifiers.
bool AstParser::ParsePtrOperators(AstDeclarator& declarator)
{
	for (;;)
	{
		const ParseToken& token = Peek();
		if (AtSimple(OP_STAR) || AtSimple(OP_XOR))
		{
			// Clang block pointers (`^`) compose as ordinary function
			// pointers for hosted compile acceptance.
			AstDeclaratorItem item;
			item.kind = DI_PTR;
			item.token = OP_STAR;
			item.spelling = token.spelling;
			Advance();
			declarator.items.push_back(move(item));
		}
		else if (AtSimple(OP_AMP) || AtSimple(OP_LAND))
		{
			AstDeclaratorItem item;
			item.kind = DI_PTR;
			item.token = token.simple_type;
			item.spelling = token.spelling;
			Advance();
			declarator.items.push_back(move(item));
			continue;  // no cv after a reference
		}
		else
		{
			// nested-name-specifier ::* (pointer to member)
			State state = Save();
			AstName qualifier;
			qualifier.global_scope = MatchSimple(OP_COLON2);
			if (!ParseNestedNameParts(qualifier) || qualifier.parts.empty() ||
			    !MatchSimple(OP_STAR))
			{
				Restore(state);
				return true;
			}
			AstDeclaratorItem item;
			item.kind = DI_MEMBER_PTR;
			item.name = move(qualifier);
			declarator.items.push_back(move(item));
		}
		for (;;)
		{
			if (AtSimple(KW_CONST) || AtSimple(KW_VOLATILE))
			{
				AstDeclaratorItem cv;
				cv.kind = DI_CV;
				cv.token = Peek().simple_type;
				cv.spelling = Peek().spelling;
				Advance();
				declarator.items.push_back(move(cv));
				continue;
			}
			// Clang nullability qualifiers and GNU restrict: hosted
			// annotations with no C++ object-model effect, dropped.
			if (AtIdentifierSpelled("_Nonnull") ||
			    AtIdentifierSpelled("_Nullable") ||
			    AtIdentifierSpelled("_Null_unspecified") ||
			    AtIdentifierSpelled("_Nullable_result") ||
			    AtIdentifierSpelled("__restrict") ||
			    AtIdentifierSpelled("__restrict__"))
			{
				Advance();
				continue;
			}
			// GNU `* __attribute__((...))` mid-declarator attributes.
			State adorned = Save();
			SkipDeclAdornments();
			if (adorned.pos == Save().pos)
				break;
		}
	}
}

// function qualifiers after a parameter clause: cv, ref-qualifier,
// noexcept, dynamic exception specification, virt-specifier, trailing
// return type. [[...]] attributes are skipped.
bool AstParser::ParseFunctionQualifiers(AstDeclarator& declarator)
{
	for (;;)
	{
		if (SkipSquareAttribute())
			continue;
		SkipDeclAdornments(&declarator.abi_tags, &declarator.asm_label);
		const ParseToken& token = Peek();
		if (AtSimple(KW_CONST) || AtSimple(KW_VOLATILE))
		{
			AstDeclaratorItem item;
			item.kind = DI_CV;
			item.token = token.simple_type;
			item.spelling = token.spelling;
			Advance();
			declarator.items.push_back(move(item));
			continue;
		}
		if (AtSimple(OP_AMP) || AtSimple(OP_LAND))
		{
			AstDeclaratorItem item;
			item.kind = DI_FUNC_QUAL;
			item.qual.kind = FQ_VIRT;
			item.qual.spelling = token.spelling;
			Advance();
			declarator.items.push_back(move(item));
			continue;
		}
		if (AtSimple(KW_NOEXCEPT))
		{
			Advance();
			AstDeclaratorItem item;
			item.kind = DI_FUNC_QUAL;
			item.qual.kind = FQ_NOEXCEPT;
			if (AtSimple(OP_LPAREN))
			{
				State state = Save();
				Advance();
				AstExprPtr expr = ParseExpression();
				if (expr && MatchSimple(OP_RPAREN))
				{
					item.qual.has_expr = true;
					item.qual.expr = move(expr);
				}
				else
					Restore(state);
			}
			declarator.items.push_back(move(item));
			continue;
		}
		if (AtSimple(KW_THROW) && AtSimple(OP_LPAREN, 1))
		{
			State state = Save();
			Advance();
			Advance();
			AstDeclaratorItem item;
			item.kind = DI_FUNC_QUAL;
			item.qual.kind = FQ_THROW;
			bool ok = true;
			if (!AtSimple(OP_RPAREN))
			{
				for (;;)
				{
					AstTypeIdPtr type;
					if (!ParseTypeId(type))
					{
						ok = false;
						break;
					}
					item.qual.throw_types.push_back(move(type));
					if (!MatchSimple(OP_COMMA))
						break;
				}
			}
			if (!ok || !MatchSimple(OP_RPAREN))
			{
				Restore(state);
				break;
			}
			declarator.items.push_back(move(item));
			continue;
		}
		if (Peek().kind == PTOK_IDENTIFIER &&
		    (Peek().HasFlag(PTF_ST_OVERRIDE) || Peek().HasFlag(PTF_ST_FINAL)))
		{
			AstDeclaratorItem item;
			item.kind = DI_FUNC_QUAL;
			item.qual.kind = FQ_VIRT;
			item.qual.spelling = token.spelling;
			Advance();
			declarator.items.push_back(move(item));
			continue;
		}
		if (AtSimple(OP_ARROW))
		{
			State state = Save();
			Advance();
			AstTypeIdPtr type;
			if (!ParseTypeId(type))
			{
				Restore(state);
				break;
			}
			AstDeclaratorItem item;
			item.kind = DI_TRAILING_RETURN;
			item.trailing_type = move(type);
			declarator.items.push_back(move(item));
			continue;
		}
		break;
	}
	return true;
}

// declarator-suffix*: parameter clauses (with their function
// qualifiers) and array bounds.
bool AstParser::ParseDeclaratorSuffixes(AstDeclarator& declarator)
{
	for (;;)
	{
		if (SkipSquareAttribute())
			continue;
		if (AtSimple(OP_LPAREN))
		{
			State state = Save();
			AstParameterClausePtr clause;
			if (!ParseParameterClause(clause))
			{
				Restore(state);
				break;
			}
			AstDeclaratorItem item;
			item.kind = DI_PARAMS;
			item.params = move(clause);
			declarator.items.push_back(move(item));
			ParseFunctionQualifiers(declarator);
			continue;
		}
		if (AtSimple(OP_LSQUARE) && !AtSimple(OP_LSQUARE, 1))
		{
			State state = Save();
			Advance();
			AstDeclaratorItem item;
			item.kind = DI_ARRAY;
			if (!AtSimple(OP_RSQUARE))
			{
				item.array_bound = ParseExpression();
				if (!item.array_bound)
				{
					Restore(state);
					break;
				}
			}
			if (!MatchSimple(OP_RSQUARE))
			{
				Restore(state);
				break;
			}
			declarator.items.push_back(move(item));
			continue;
		}
		break;
	}
	return true;
}

// direct-declarator / direct-abstract-declarator. The named nested
// reading `( identifier )` is rejected when the identifier's nearest
// declaration is a type: `future_error(error_code);` keeps error_code
// a parameter type while `foo(x);` declares x (6.8, 8.2).
bool AstParser::ParseDirectDeclarator(AstDeclarator& declarator, bool named)
{
	if (AtSimple(OP_LPAREN))
	{
		State state = Save();
		Advance();
		AstDeclaratorPtr inner;
		if (ParseDeclarator(inner, named) && inner && !inner->Empty() &&
		    MatchSimple(OP_RPAREN))
		{
			bool bare_type_id = false;
			if (named && inner->items.size() == 1 &&
			    inner->items[0].kind == DI_ID)
			{
				const AstName& id = inner->items[0].name;
				if (id.IsPlainIdentifier())
				{
					int flags = ResolveName(id);
					bare_type_id = flags != kUnresolved &&
						(flags & NF_TYPE);
				}
				else if (!id.parts.empty() &&
				         id.parts.back().kind == NP_TEMPLATE_ID)
					// PA25 8.2: `( qualified-template-id )` reads as
					// a parameter type - the function declaration
					// wins over an object declarator.
					bare_type_id = true;
			}
			if (!bare_type_id)
			{
				AstDeclaratorItem item;
				item.kind = DI_NESTED;
				item.nested = move(inner);
				declarator.items.push_back(move(item));
				return ParseDeclaratorSuffixes(declarator);
			}
		}
		Restore(state);
		if (named)
			return false;
		return ParseDeclaratorSuffixes(declarator);
	}
	if (AtSimple(OP_DOTS))
	{
		// Parameter pack marker before the (optional) declarator-id.
		State state = Save();
		Advance();
		AstDeclaratorItem pack;
		pack.kind = DI_PACK;
		declarator.items.push_back(move(pack));
		if (AtIdentifier())
		{
			AstName name;
			if (ParseIdExpressionName(name))
			{
				AstDeclaratorItem item;
				item.kind = DI_ID;
				item.name = move(name);
				declarator.items.push_back(move(item));
			}
		}
		else if (named)
		{
			Restore(state);
			declarator.items.pop_back();
			return false;
		}
		return ParseDeclaratorSuffixes(declarator);
	}
	if (named)
	{
		State state = Save();
		AstName name;
		if (!ParseIdExpressionName(name))
			return false;
		// Destructors and conversion functions are declared through the
		// special-member grammar, never as ordinary declarator-ids.
		if (name.parts.back().tilde ||
		    name.parts.back().kind == NP_CONVERSION_FUNCTION)
		{
			Restore(state);
			return false;
		}
		AstDeclaratorItem item;
		item.kind = DI_ID;
		item.name = move(name);
		declarator.items.push_back(move(item));
		if (!ParseDeclaratorSuffixes(declarator))
		{
			Restore(state);
			return false;
		}
		return true;
	}
	return ParseDeclaratorSuffixes(declarator);
}

// declarator: ptr-operator* direct-declarator. For named declarators
// failure restores entry state; abstract declarators always succeed,
// possibly empty (the caller checks Empty()).
bool AstParser::ParseDeclarator(AstDeclaratorPtr& declarator, bool named)
{
	State state = Save();
	AstDeclaratorPtr result(new AstDeclarator());
	ParsePtrOperators(*result);
	if (!ParseDirectDeclarator(*result, named))
	{
		Restore(state);
		return false;
	}
	if (named && !result->IdName())
	{
		Restore(state);
		return false;
	}
	declarator = move(result);
	return true;
}

bool AstParser::AtParameterFollow() const
{
	return AtSimple(OP_COMMA) || AtSimple(OP_RPAREN) || AtSimple(OP_ASS) ||
		AtSimple(OP_DOTS) || AtCloseAngle();
}

// parameter-declaration: decl-specifier-seq with an abstract-first
// declarator (8.2/7: the type-id reading wins when both parse), the
// choice validated against the parameter follow set.
bool AstParser::ParseParameterDeclaration(AstParameter& parameter)
{
	State state = Save();
	if (!ParseSpecifierSeq(parameter.specifiers, kDeclSpecifierSeq))
	{
		Restore(state);
		return false;
	}
	State declarator_state = Save();
	AstDeclaratorPtr abstract;
	bool have = false;
	if (ParseDeclarator(abstract, false) &&
	    (SkipDeclAdornments(), AtParameterFollow()))
	{
		if (!abstract->Empty())
			parameter.declarator = move(abstract);
		have = true;
	}
	if (!have)
	{
		Restore(declarator_state);
		AstDeclaratorPtr named;
		// PA34: post-declarator parameter attributes
		// (`name __attribute__((...))`) drop before the follow check.
		if (ParseDeclarator(named, true) &&
		    (SkipDeclAdornments(), AtParameterFollow()))
			parameter.declarator = move(named);
		else
		{
			Restore(state);
			return false;
		}
	}
	if (AtSimple(OP_ASS))
	{
		Advance();
		AstExprPtr value = ParseInitializerClause();
		if (!value)
		{
			Restore(state);
			return false;
		}
		AstInitializerPtr init(new AstInitializer());
		init->kind = INIT_EQ;
		init->expr = move(value);
		parameter.default_arg = move(init);
	}
	return true;
}

// parameter-clause: ( parameter-declaration-list? ) with the trailing
// `, ...` and lone `...` variadic forms.
bool AstParser::ParseParameterClause(AstParameterClausePtr& clause)
{
	State state = Save();
	if (!MatchSimple(OP_LPAREN))
		return false;
	AstParameterClausePtr result(new AstParameterClause());
	if (MatchSimple(OP_RPAREN))
	{
		clause = move(result);
		return true;
	}
	if (MatchSimple(OP_DOTS))
	{
		if (!MatchSimple(OP_RPAREN))
		{
			Restore(state);
			return false;
		}
		result->variadic = true;
		clause = move(result);
		return true;
	}
	for (;;)
	{
		AstParameter parameter;
		if (!ParseParameterDeclaration(parameter))
		{
			Restore(state);
			return false;
		}
		result->parameters.push_back(move(parameter));
		if (MatchSimple(OP_COMMA))
		{
			if (MatchSimple(OP_DOTS))
			{
				result->variadic = true;
				break;
			}
			continue;
		}
		if (MatchSimple(OP_DOTS))
		{
			result->variadic = true;
			break;
		}
		break;
	}
	if (!MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return false;
	}
	clause = move(result);
	return true;
}

// init-declarator initializers: = initializer-clause (with = default
// and = delete as special members), braced-init-list, and when
// allowed, ( expression-list ).
bool AstParser::ParseInitializer(AstInitializerPtr& init, bool allow_paren)
{
	if (AtSimple(OP_ASS))
	{
		Advance();
		AstInitializerPtr result(new AstInitializer());
		if (AtSimple(KW_DEFAULT))
		{
			Advance();
			result->kind = INIT_DEFAULT;
			init = move(result);
			return true;
		}
		if (AtSimple(KW_DELETE))
		{
			Advance();
			result->kind = INIT_DELETE;
			init = move(result);
			return true;
		}
		AstExprPtr value = ParseInitializerClause();
		if (!value)
			return false;
		result->kind = INIT_EQ;
		result->expr = move(value);
		init = move(result);
		return true;
	}
	if (AtSimple(OP_LBRACE))
	{
		AstExprPtr braced = ParseBracedInitList();
		if (!braced)
			return false;
		AstInitializerPtr result(new AstInitializer());
		result->kind = INIT_BRACED;
		result->expr = move(braced);
		init = move(result);
		return true;
	}
	if (allow_paren && AtSimple(OP_LPAREN))
	{
		State state = Save();
		Advance();
		AstInitializerPtr result(new AstInitializer());
		result->kind = INIT_PAREN;
		if (!ParseInitializerClauseList(result->args) ||
		    !MatchSimple(OP_RPAREN))
		{
			Restore(state);
			return false;
		}
		init = move(result);
		return true;
	}
	return false;
}

bool AstParser::ParseBraceOrEqualInitializer(AstInitializerPtr& init)
{
	if (!AtSimple(OP_ASS) && !AtSimple(OP_LBRACE))
		return false;
	return ParseInitializer(init, false);
}
