#include "ast/ast_parser.h"

using std::string;
using std::vector;
using std::move;

// --- template-id policy --------------------------------------------------
//
// `identifier <` starts a simple-template-id attempt when the name's
// classification allows it:
//  - resolved with the template category: attempt, commit on syntactic
//    success;
//  - resolved without it (a variable, parameter, plain typedef): never
//    attempt, `<` is less-than;
//  - unresolved: attempt; in type contexts commit on success (there is
//    no less-than reading of a type), in expression contexts commit
//    only when the template-id is immediately followed by `(`.
// Inside a nested-name-specifier an attempt is committed only when
// `::` follows the closing angle, which makes it self-checking.

bool AstParser::TemplateIdAllowed(int flags) const
{
	return flags == kUnresolved || (flags & NF_TEMPLATE) != 0;
}

bool AstParser::AtTemplateArgumentFollow() const
{
	return AtSimple(OP_COMMA) || AtCloseAngle();
}

// template-argument: type-id | assignment-expression, with the type
// reading preferred and committed only at a list boundary (8.2/3).
bool AstParser::ParseTemplateArgument(AstTemplateArgument& argument)
{
	State state = Save();
	AstTypeIdPtr type;
	if (ParseTypeId(type))
	{
		argument.pack = MatchSimple(OP_DOTS);
		if (AtTemplateArgumentFollow())
		{
			argument.is_type = true;
			argument.type = move(type);
			return true;
		}
	}
	Restore(state);
	argument.pack = false;
	AstExprPtr expr = ParseAssignmentExpression();
	if (!expr)
	{
		Restore(state);
		return false;
	}
	argument.is_type = false;
	argument.expr = move(expr);
	argument.pack = MatchSimple(OP_DOTS);
	return true;
}

// < template-argument-list? close-angle-bracket; the part's identifier
// is already filled in by the caller.
bool AstParser::ParseTemplateArgumentClause(AstNamePart& part)
{
	State state = Save();
	if (!MatchOpenAngle())
		return false;
	part.kind = NP_TEMPLATE_ID;
	if (MatchCloseAngle())
		return true;
	for (;;)
	{
		AstTemplateArgument argument;
		if (!ParseTemplateArgument(argument))
		{
			Restore(state);
			part.kind = NP_IDENTIFIER;
			part.arguments.clear();
			return false;
		}
		part.arguments.push_back(move(argument));
		if (MatchSimple(OP_COMMA))
			continue;
		break;
	}
	if (!MatchCloseAngle())
	{
		Restore(state);
		part.kind = NP_IDENTIFIER;
		part.arguments.clear();
		return false;
	}
	return true;
}

// Accumulates `X::` qualifier parts (identifier, simple-template-id,
// or decltype roots; `template` allowed before later parts). Each part
// commits only when its trailing `::` is present.
bool AstParser::ParseNestedNameParts(AstName& name)
{
	for (;;)
	{
		State state = Save();
		AstNamePart part;
		bool matched = false;
		if (AtSimple(KW_DECLTYPE) && name.parts.empty())
		{
			Advance();
			if (MatchSimple(OP_LPAREN))
			{
				AstExprPtr expr = ParseExpression();
				if (expr && MatchSimple(OP_RPAREN))
				{
					part.kind = NP_DECLTYPE;
					part.decltype_expr = move(expr);
					matched = MatchSimple(OP_COLON2);
				}
			}
		}
		else
		{
			if (AtSimple(KW_TEMPLATE) && !name.parts.empty() &&
			    AtIdentifier(1))
			{
				Advance();
				part.template_keyword = true;
			}
			if (AtIdentifier())
			{
				part.identifier = Peek().spelling;
				Advance();
				if (AtSimple(OP_LT))
				{
					State id_state = Save();
					if (!ParseTemplateArgumentClause(part) ||
					    !AtSimple(OP_COLON2))
					{
						Restore(id_state);
						part.kind = NP_IDENTIFIER;
						part.arguments.clear();
					}
				}
				matched = MatchSimple(OP_COLON2);
			}
		}
		if (!matched)
		{
			Restore(state);
			return true;
		}
		name.parts.push_back(move(part));
	}
}

// Resolves the classification of `prefix-parts :: id` (or plain `id`
// when the name built so far has no qualifiers).
int AstParser::ResolveTerminalFlags(const AstName& prefix,
                                    const string& id) const
{
	if (prefix.parts.empty())
	{
		if (prefix.global_scope)
		{
			std::map<string, unsigned>::const_iterator it =
				scopes_[0].table->entries.find(id);
			if (it != scopes_[0].table->entries.end())
				return static_cast<int>(it->second);
			return kUnresolved;
		}
		return LookupUnqualified(id);
	}
	const NameTable* table = DescendPrefix(prefix);
	if (!table)
		return kUnresolved;
	std::map<string, unsigned>::const_iterator it = table->entries.find(id);
	if (it == table->entries.end())
		return kUnresolved;
	return static_cast<int>(it->second);
}

// Parses the terminal identifier (possibly a simple-template-id) of a
// name under the attempt policy above. An explicit `template` keyword
// after a qualifier asserts templateness (14.2/4).
bool AstParser::ParseTerminalIdentifier(AstName& name, bool type_context)
{
	bool template_keyword = false;
	if (AtSimple(KW_TEMPLATE) && AtIdentifier(1) && !name.parts.empty())
	{
		Advance();
		template_keyword = true;
	}
	if (!AtIdentifier())
		return false;
	AstNamePart part;
	part.identifier = Peek().spelling;
	part.template_keyword = template_keyword;
	Advance();
	if (AtSimple(OP_LT))
	{
		int flags = template_keyword ? (NF_TYPE | NF_TEMPLATE)
		                             : ResolveTerminalFlags(name, part.identifier);
		if (TemplateIdAllowed(flags))
		{
			State state = Save();
			if (ParseTemplateArgumentClause(part))
			{
				bool commit = (flags != kUnresolved) || type_context ||
					AtSimple(OP_LPAREN);
				if (!commit)
				{
					Restore(state);
					part.kind = NP_IDENTIFIER;
					part.arguments.clear();
				}
			}
		}
	}
	name.parts.push_back(move(part));
	return true;
}

// conversion-type-id: type-specifier-seq ptr-operator* (no nested
// declarators or parameter clauses; `operator int()`'s parens belong
// to the member declarator).
bool AstParser::ParseConversionTypeId(AstTypeIdPtr& type)
{
	AstTypeIdPtr result(new AstTypeId());
	if (!ParseSpecifierSeq(result->specifiers, kTypeSpecifierSeq))
		return false;
	AstDeclaratorPtr declarator(new AstDeclarator());
	ParsePtrOperators(*declarator);
	if (!declarator->Empty())
		result->declarator = move(declarator);
	type = move(result);
	return true;
}

// operator-function-id, conversion-function-id, or
// literal-operator-id; the lookahead sits on KW_OPERATOR.
bool AstParser::ParseOperatorIdPart(AstNamePart& part)
{
	if (!MatchSimple(KW_OPERATOR))
		return false;
	if (AtLiteral() && Peek().HasFlag(PTF_ST_EMPTYSTR) && AtIdentifier(1))
	{
		Advance();
		part.kind = NP_LITERAL_OPERATOR;
		part.identifier = Peek().spelling;
		Advance();
		return true;
	}
	if (AtSimple(KW_NEW) || AtSimple(KW_DELETE))
	{
		part.kind = NP_OPERATOR_FUNCTION;
		part.operator_text = Peek().spelling;
		Advance();
		if (AtSimple(OP_LSQUARE) && AtSimple(OP_RSQUARE, 1))
		{
			Advance();
			Advance();
			part.operator_text += "[]";
		}
		return true;
	}
	if (AtSimple(OP_LPAREN) && AtSimple(OP_RPAREN, 1))
	{
		Advance();
		Advance();
		part.kind = NP_OPERATOR_FUNCTION;
		part.operator_text = "()";
		return true;
	}
	if (AtSimple(OP_LSQUARE) && AtSimple(OP_RSQUARE, 1))
	{
		Advance();
		Advance();
		part.kind = NP_OPERATOR_FUNCTION;
		part.operator_text = "[]";
		return true;
	}
	const ParseToken& token = Peek();
	if (token.kind == PTOK_RSHIFT_1 && Peek(1).kind == PTOK_RSHIFT_2)
	{
		Advance();
		Advance();
		part.kind = NP_OPERATOR_FUNCTION;
		part.operator_text = ">>";
		return true;
	}
	if (token.kind == PTOK_SIMPLE)
	{
		switch (token.simple_type)
		{
		case OP_PLUS: case OP_MINUS: case OP_STAR: case OP_DIV:
		case OP_MOD: case OP_XOR: case OP_AMP: case OP_BOR:
		case OP_COMPL: case OP_LNOT: case OP_ASS: case OP_LT:
		case OP_GT: case OP_PLUSASS: case OP_MINUSASS: case OP_STARASS:
		case OP_DIVASS: case OP_MODASS: case OP_XORASS: case OP_BANDASS:
		case OP_BORASS: case OP_LSHIFT: case OP_LSHIFTASS:
		case OP_RSHIFTASS: case OP_EQ: case OP_NE: case OP_LE:
		case OP_GE: case OP_LAND: case OP_LOR: case OP_INC:
		case OP_DEC: case OP_COMMA: case OP_ARROWSTAR: case OP_ARROW:
			part.kind = NP_OPERATOR_FUNCTION;
			part.operator_text = token.spelling;
			Advance();
			return true;
		default:
			break;
		}
	}
	// conversion-function-id
	AstTypeIdPtr type;
	if (!ParseConversionTypeId(type))
		return false;
	part.kind = NP_CONVERSION_FUNCTION;
	part.conversion_type = move(type);
	return true;
}

// id-expression: optionally qualified, terminal unqualified-id of any
// form (identifier, template-id, ~identifier, operator ids).
bool AstParser::ParseIdExpressionName(AstName& name)
{
	State state = Save();
	name.global_scope = MatchSimple(OP_COLON2);
	if (!ParseNestedNameParts(name))
	{
		Restore(state);
		return false;
	}
	if (AtSimple(OP_COMPL) && AtIdentifier(1))
	{
		Advance();
		AstNamePart part;
		part.tilde = true;
		part.identifier = Peek().spelling;
		Advance();
		if (AtSimple(OP_LT))
			ParseTemplateArgumentClause(part);
		name.parts.push_back(move(part));
		return true;
	}
	if (AtSimple(KW_OPERATOR))
	{
		AstNamePart part;
		if (!ParseOperatorIdPart(part))
		{
			Restore(state);
			return false;
		}
		name.parts.push_back(move(part));
		return true;
	}
	if (ParseTerminalIdentifier(name, false))
		return true;
	Restore(state);
	return false;
}

// qualified-type-name: optionally `typename`-prefixed qualified name
// whose terminal is an identifier or simple-template-id. The caller
// checks NameUsableAsType for the value-shadowing rule.
bool AstParser::ParseQualifiedTypeName(AstName& name)
{
	State state = Save();
	name.typename_keyword = MatchSimple(KW_TYPENAME);
	name.global_scope = MatchSimple(OP_COLON2);
	ParseNestedNameParts(name);
	if (name.typename_keyword && name.parts.empty())
	{
		// typename requires a qualified name (14.6).
		Restore(state);
		return false;
	}
	if (!ParseTerminalIdentifier(name, true))
	{
		Restore(state);
		return false;
	}
	return true;
}
