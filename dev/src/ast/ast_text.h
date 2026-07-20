#pragma once

#include <string>

#include "ast/ast.h"

// One-line spellings of structured AST names, types, and expressions
// for the PA10 dump annotations (`identifier Box<T>::~Box`,
// `decl-specifier pointer<Y*,...>`, `placement ((void*)p)`). These are
// renderings of the typed tree; nothing parses them back.

// Renders a name with its typename/template keywords preserved (the
// form used inside template arguments).
std::string FlattenName(const AstName& name);

// Renders a name in a top-level specifier or declarator-id position:
// a leading `typename` keyword is dropped.
std::string FlattenNameTopLevel(const AstName& name);

// Renders one name component as it appears after a
// nested-name-specifier (a qualified conversion-function-id spells
// `operator type` with a space).
std::string FlattenQualifiedNamePart(const AstNamePart& part);

// Renders the "[...]" capture text of a lambda-introducer.
std::string FlattenLambdaIntroducer(const AstLambda& lambda);

std::string FlattenTypeId(const AstTypeId& type);
std::string FlattenSpecifier(const AstSpecifier& specifier);
std::string FlattenSpecifierSeq(const AstSpecifierSeq& specifiers);
std::string FlattenDeclarator(const AstDeclarator& declarator);
std::string FlattenExpr(const AstExpr& expr);
std::string FlattenExprList(const std::vector<AstExprPtr>& list);
