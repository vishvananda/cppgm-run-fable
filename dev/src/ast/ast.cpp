#include "ast/ast.h"

AstTemplateArgument::AstTemplateArgument()
	: is_type(false), pack(false)
{
}

AstNamePart::AstNamePart()
	: kind(NP_IDENTIFIER), tilde(false), template_keyword(false)
{
}

AstName::AstName()
	: global_scope(false), typename_keyword(false)
{
}

bool AstName::IsPlainIdentifier() const
{
	return !global_scope && !typename_keyword && parts.size() == 1 &&
		parts[0].kind == NP_IDENTIFIER && !parts[0].tilde;
}

AstSpecifier::AstSpecifier()
	: kind(SPEC_KEYWORD), keyword(KW_ALIGNAS)
{
}

bool AstSpecifier::IsCv() const
{
	return kind == SPEC_KEYWORD &&
		(keyword == KW_CONST || keyword == KW_VOLATILE);
}

AstFunctionQualifier::AstFunctionQualifier()
	: kind(FQ_NOEXCEPT), has_expr(false)
{
}

AstDeclaratorItem::AstDeclaratorItem()
	: kind(DI_PTR), token(OP_STAR)
{
}

// The declarator-id of a (possibly parenthesized) declarator: the
// DI_ID item at any nesting depth, or null for abstract declarators.
const AstName* AstDeclarator::IdName() const
{
	for (size_t i = 0; i < items.size(); i++)
	{
		if (items[i].kind == DI_ID)
			return &items[i].name;
		if (items[i].kind == DI_NESTED && items[i].nested)
		{
			const AstName* inner = items[i].nested->IdName();
			if (inner)
				return inner;
		}
	}
	return 0;
}

AstParameterClause::AstParameterClause()
	: variadic(false)
{
}

AstInitializer::AstInitializer()
	: kind(INIT_EQ)
{
}

AstExpr::AstExpr(EExprKind kind_in)
	: kind(kind_in), op(OP_STAR), is_type_operand(false),
	  sizeof_paren(false), global_scope(false), array_delete(false),
	  has_placement(false), paren_type(false)
{
}

AstLambdaCapture::AstLambdaCapture()
	: kind(LC_COPY), pack(false)
{
}

AstLambda::AstLambda()
	: has_capture_default(false), capture_default(OP_AMP),
	  has_declarator(false), mutable_specifier(false),
	  has_noexcept(false), has_trailing(false)
{
}

AstCondition::AstCondition()
	: is_declaration(false)
{
}

AstHandler::AstHandler()
	: ellipsis(false)
{
}

AstStmt::AstStmt(EStmtKind kind_in)
	: kind(kind_in)
{
}

AstBaseSpecifier::AstBaseSpecifier()
	: is_virtual(false), has_access(false), access(KW_PUBLIC), pack(false)
{
}

AstTemplateParameter::AstTemplateParameter()
	: kind(TP_TYPE), key(KW_CLASS), pack(false), has_default_type(false),
	  has_default_expr(false), default_token_form(false)
{
}

AstMemberSpecifier::AstMemberSpecifier()
	: keyword(KW_INLINE)
{
}

AstDecl::AstDecl(EDeclKind kind_in)
	: kind(kind_in), inline_namespace(false), unnamed(false),
	  has_name(false), class_key(KW_CLASS), has_enum_key(false),
	  enum_key(KW_CLASS), has_enum_base(false), has_message(false),
	  access(KW_PUBLIC), has_ctor_initializer(false),
	  has_parameter_list(false)
{
}
