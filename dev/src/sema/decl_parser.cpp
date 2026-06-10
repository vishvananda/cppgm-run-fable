#include "sema/decl_parser.h"

#include "sema/init.h"
#include "sema/program.h"

using std::runtime_error;
using std::to_string;

namespace {

// True when the keyword can begin a parameter-declaration's
// decl-specifier-seq (the '(' disambiguation).
bool IsDeclSpecifierKeyword(ETokenType type)
{
	switch (type)
	{
	case KW_STATIC:
	case KW_THREAD_LOCAL:
	case KW_EXTERN:
	case KW_TYPEDEF:
	case KW_CONSTEXPR:
	case KW_INLINE:
	case KW_CONST:
	case KW_VOLATILE:
	case KW_SIGNED:
	case KW_UNSIGNED:
	case KW_SHORT:
	case KW_LONG:
	case KW_CHAR:
	case KW_CHAR16_T:
	case KW_CHAR32_T:
	case KW_WCHAR_T:
	case KW_BOOL:
	case KW_INT:
	case KW_FLOAT:
	case KW_DOUBLE:
	case KW_VOID:
		return true;
	default:
		return false;
	}
}

bool IsUnqualifiedVoid(const TypePtr& type)
{
	return IsVoidType(type) && !type->is_const && !type->is_volatile;
}

// The function-signature part of a linking key (3.5: functions link by
// name and parameter types; the canonical recursive description is the
// stable rendering of the already-adjusted parameter list).
string SignatureKey(const TypePtr& type)
{
	string key = "(";
	for (size_t i = 0; i < type->parameters.size(); i++)
	{
		if (i > 0)
			key += ",";
		key += DescribeType(type->parameters[i]);
	}
	if (type->variadic)
		key += ",...";
	return key + ")";
}

bool SameSignature(const TypePtr& a, const TypePtr& b)
{
	if (a->variadic != b->variadic ||
	    a->parameters.size() != b->parameters.size())
		return false;
	for (size_t i = 0; i < a->parameters.size(); i++)
		if (!TypeEquals(a->parameters[i], b->parameters[i]))
			return false;
	return true;
}

vector<Namespace*> NamespaceChain(Namespace* ns)
{
	vector<Namespace*> chain;
	for (Namespace* walk = ns; walk; walk = walk->parent)
		chain.push_back(walk);
	return vector<Namespace*>(chain.rbegin(), chain.rend());
}

} // namespace

DeclParser::DeclParser(const vector<PostToken>& tokens, SemaModel& model,
                       Program& program)
	: tokens_(tokens), pos_(0), model_(model), program_(program),
	  scope_switched_(false)
{
	eof_.kind = PTK_EOF;
	for (size_t i = 0; i < tokens_.size(); i++)
		if (tokens_[i].kind == PTK_INVALID)
			throw runtime_error("invalid token at phase 7: " +
			                    tokens_[i].source);
}

void DeclParser::ParseTranslationUnit()
{
	program_.BeginTranslationUnit();
	scopes_.assign(1, model_.global());
	ParseDeclarationSeq();
	if (Peek().kind != PTK_EOF)
		throw ParseError("expected declaration");
}

// --- token stream ---

const PostToken& DeclParser::Peek(size_t ahead) const
{
	size_t index = pos_ + ahead;
	if (index >= tokens_.size())
		return eof_;
	return tokens_[index];
}

void DeclParser::Advance()
{
	if (pos_ < tokens_.size())
		pos_++;
}

bool DeclParser::AtSimple(ETokenType type, size_t ahead) const
{
	const PostToken& token = Peek(ahead);
	return token.kind == PTK_SIMPLE && token.token_type == type;
}

bool DeclParser::AtIdentifier(size_t ahead) const
{
	return Peek(ahead).kind == PTK_IDENTIFIER;
}

void DeclParser::ExpectSimple(ETokenType type)
{
	if (!AtSimple(type))
		throw ParseError("expected " + TokenTypeName(type));
	Advance();
}

string DeclParser::ExpectIdentifier()
{
	if (!AtIdentifier())
		throw ParseError("expected identifier");
	string spelling = Peek().source;
	Advance();
	return spelling;
}

runtime_error DeclParser::ParseError(const string& message) const
{
	const PostToken& token = Peek();
	string at = token.kind == PTK_EOF ? string("end of input")
	                                  : "`" + token.source + "`";
	return runtime_error("parse error: " + message + " at " + at);
}

// --- declarations ---

void DeclParser::ParseDeclarationSeq()
{
	while (!AtSimple(OP_RBRACE) && Peek().kind != PTK_EOF)
		ParseDeclaration();
}

void DeclParser::ParseDeclaration()
{
	if (AtSimple(OP_SEMICOLON))
	{
		Advance();  // empty-declaration
		return;
	}
	if (AtSimple(KW_INLINE) && AtSimple(KW_NAMESPACE, 1))
	{
		ParseNamespaceDefinition();
		return;
	}
	if (AtSimple(KW_NAMESPACE))
	{
		if (AtIdentifier(1) && AtSimple(OP_ASS, 2))
			ParseNamespaceAlias();
		else
			ParseNamespaceDefinition();
		return;
	}
	if (AtSimple(KW_USING))
	{
		if (AtSimple(KW_NAMESPACE, 1))
			ParseUsingDirective();
		else if (AtIdentifier(1) && AtSimple(OP_ASS, 2))
			ParseAliasDeclaration();
		else
			ParseUsingDeclaration();
		return;
	}
	if (AtSimple(KW_STATIC_ASSERT))
	{
		ParseStaticAssert();
		return;
	}
	ParseSimpleDeclaration();
}

void DeclParser::ParseNamespaceDefinition()
{
	bool is_inline = false;
	if (AtSimple(KW_INLINE))
	{
		is_inline = true;
		Advance();
	}
	ExpectSimple(KW_NAMESPACE);
	string name;
	bool is_unnamed = true;
	if (AtIdentifier())
	{
		name = ExpectIdentifier();
		is_unnamed = false;
	}
	ExpectSimple(OP_LBRACE);
	Namespace& parent = *scopes_.back();
	Namespace* ns = 0;
	if (is_unnamed)
		ns = parent.unnamed_member;
	else
	{
		map<string, Binding>::iterator it = parent.bindings.find(name);
		if (it != parent.bindings.end())
		{
			// 7.3.1/3.3.1: only the original member namespace can be
			// extended - not an alias (7.3.2) or any other entity.
			if (it->second.kind != BK_NAMESPACE || it->second.imported)
				throw ParseError("`" + name +
				                 "` redeclared as a namespace");
			ns = it->second.target;
		}
	}
	if (ns)
	{
		// 7.3.1p9: inline may be used on an extension-namespace-
		// definition only if it was used on the original.
		if (is_inline && !ns->is_inline)
			throw ParseError("namespace `" + name +
			                 "` was not declared inline");
	}
	else
	{
		ns = AddMemberNamespace(model_, parent, name, is_inline);
		// Unnamed namespaces (and everything inside one) never link, so
		// they keep the default negative id - which doubles as the
		// internal-linkage-context fact of 3.5p4.
		if (!name.empty() && parent.link_id >= 0)
			ns->link_id = program_.InternNamespace(parent.link_id, name);
		lookup_cache_.Invalidate();
	}
	scopes_.push_back(ns);
	ParseDeclarationSeq();
	ExpectSimple(OP_RBRACE);
	scopes_.pop_back();
}

void DeclParser::ParseNamespaceAlias()
{
	ExpectSimple(KW_NAMESPACE);
	string name = ExpectIdentifier();
	ExpectSimple(OP_ASS);
	Namespace* target = ParseNamespaceSpecifier();
	ExpectSimple(OP_SEMICOLON);
	Namespace& current = *scopes_.back();
	map<string, Binding>::iterator it = current.bindings.find(name);
	if (it != current.bindings.end())
	{
		// 7.3.2p3: may only redefine an alias declared here, to the
		// namespace it already refers to.
		if (it->second.kind != BK_NAMESPACE || !it->second.imported ||
		    it->second.target != target)
			throw ParseError("namespace alias `" + name +
			                 "` conflicts with a previous declaration");
		return;
	}
	Binding binding;
	binding.kind = BK_NAMESPACE;
	binding.target = target;
	binding.imported = true;
	current.bindings[name] = binding;
}

void DeclParser::ParseUsingDirective()
{
	ExpectSimple(KW_USING);
	ExpectSimple(KW_NAMESPACE);
	Namespace* target = ParseNamespaceSpecifier();
	ExpectSimple(OP_SEMICOLON);
	AddUsingDirective(*scopes_.back(), target);
	lookup_cache_.Invalidate();
}

void DeclParser::ParseUsingDeclaration()
{
	ExpectSimple(KW_USING);
	QualifiedName name = ParseIdExpression();
	ExpectSimple(OP_SEMICOLON);
	if (!name.Qualified())
		throw ParseError("using-declaration requires a qualified name");
	Binding found;
	if (!ResolveComponents(name.root_global, name.path, name.name,
	                       LF_ANY_ENTITY, found))
		throw ParseError("using-declaration names unknown `" + name.name +
		                 "`");
	if (found.kind == BK_NAMESPACE)
		throw ParseError("using-declaration shall not name a namespace");
	found.imported = true;
	for (size_t i = 0; i < found.overloads.size(); i++)
		found.overloads[i].imported = true;
	Namespace& current = *scopes_.back();
	map<string, Binding>::iterator it = current.bindings.find(name.name);
	if (it == current.bindings.end())
	{
		// The current namespace re-binds the same entity; it still
		// prints only under its first namespace.
		current.bindings[name.name] = found;
		return;
	}
	Binding& existing = it->second;
	if (existing.kind == BK_FUNCTION && found.kind == BK_FUNCTION)
	{
		// 7.3.3p14: the imported overloads extend the set unless one
		// clashes with a different function of the same signature.
		for (size_t i = 0; i < found.overloads.size(); i++)
		{
			DeclaredEntity* incoming = found.overloads[i].entity;
			bool present = false;
			for (size_t j = 0; j < existing.overloads.size(); j++)
			{
				DeclaredEntity* prior = existing.overloads[j].entity;
				if (prior == incoming)
					present = true;
				else if (SameSignature(prior->type, incoming->type))
					throw ParseError("using-declaration of `" + name.name +
					                 "` conflicts with a previous "
					                 "declaration");
			}
			if (!present)
				existing.overloads.push_back(found.overloads[i]);
		}
		return;
	}
	bool same = existing.kind == found.kind &&
		((existing.kind == BK_VARIABLE && existing.entity == found.entity) ||
		 (existing.kind == BK_TYPEDEF &&
		  TypeEquals(existing.type, found.type)));
	if (!same)
		throw ParseError("using-declaration of `" + name.name +
		                 "` conflicts with a previous declaration");
}

void DeclParser::ParseAliasDeclaration()
{
	ExpectSimple(KW_USING);
	string name = ExpectIdentifier();
	ExpectSimple(OP_ASS);
	TypePtr type = ParseTypeId();
	ExpectSimple(OP_SEMICOLON);
	BindTypedef(name, type);
}

void DeclParser::ParseStaticAssert()
{
	ExpectSimple(KW_STATIC_ASSERT);
	ExpectSimple(OP_LPAREN);
	Expr condition = ParseExpression();
	ExpectSimple(OP_COMMA);
	// The message is a literal token of the grammar, not an expression:
	// it gets no string-literal image object.
	const PostToken& message = Peek();
	if (message.kind != PTK_LITERAL && message.kind != PTK_LITERAL_ARRAY)
		throw ParseError("expected literal");
	string text = message.source;
	Advance();
	ExpectSimple(OP_RPAREN);
	ExpectSimple(OP_SEMICOLON);
	if (!EvaluateStaticAssertCondition(condition, program_.current_tu()))
		throw runtime_error("static_assert failed: " + text);
}

void DeclParser::ParseSimpleDeclaration()
{
	DeclSpecifiers specs = ParseDeclSpecifierSeq(SC_DECLARATION);
	bool first = true;
	while (true)
	{
		Declarator declarator;
		ParseDeclarator(DM_NAMED, declarator);
		if (first && AtSimple(OP_LBRACE))
		{
			ParseFunctionDefinition(specs, declarator);
			RestoreDeclaratorScope();
			return;
		}
		first = false;
		DeclaredEntity* entity = DeclareSimple(specs, declarator);
		if (AtSimple(OP_ASS))
		{
			Advance();
			if (!entity || entity->kind != SLOT_VARIABLE)
				throw ParseError("only a variable can have an initializer");
			Expr init = ParseExpression();
			program_.RecordDefinition(entity);
			ApplyInitializer(program_, *entity, init, entity->name);
		}
		else
			FinishUninitializedDeclaration(specs, entity);
		RestoreDeclaratorScope();
		if (!AtSimple(OP_COMMA))
			break;
		Advance();
	}
	ExpectSimple(OP_SEMICOLON);
}

void DeclParser::ParseFunctionDefinition(const DeclSpecifiers& specs,
                                         const Declarator& declarator)
{
	DeclaredEntity* entity = DeclareSimple(specs, declarator);
	if (!entity || entity->kind != SLOT_FUNCTION)
		throw ParseError("only a function may have a body");
	// 8.3.5p10: a typedef of function type shall not be used to define a
	// function. A chunk-free declarator of function type means the whole
	// type came from the decl-specifier-seq.
	if (!HasDeclaratorChunks(declarator))
		throw ParseError("function `" + entity->name +
		                 "` defined through a typedef");
	program_.RecordDefinition(entity);
	ExpectSimple(OP_LBRACE);
	ExpectSimple(OP_RBRACE);
}

void DeclParser::FinishUninitializedDeclaration(const DeclSpecifiers& specs,
                                                DeclaredEntity* entity)
{
	if (!entity || entity->kind != SLOT_VARIABLE)
		return;  // typedef or function declaration
	if (specs.is_extern)
	{
		// A declaration, not a definition (3.1p2) - but constexpr
		// objects must be initialized wherever declared (7.1.5p9).
		if (specs.is_constexpr)
			throw ParseError("constexpr variable `" + entity->name +
			                 "` requires an initializer");
		return;
	}
	program_.RecordDefinition(entity);
	ApplyDefaultInitialization(*entity, entity->name);
}

DeclaredEntity* DeclParser::DeclareSimple(const DeclSpecifiers& specs,
                                          const Declarator& declarator)
{
	const QualifiedName* name = DeclaratorName(declarator);
	if (!name)
		throw ParseError("declarator requires a declarator-id");
	TypePtr type = ComputeDeclaratorType(specs.type, declarator);
	if (specs.is_typedef)
	{
		if (name->Qualified())
			throw ParseError("typedef declarator-id shall not be qualified");
		BindTypedef(name->name, type);
		return 0;
	}
	if (type->kind == TK_FUNCTION)
	{
		if (specs.is_thread_local)
			throw ParseError("thread_local on function `" + name->name +
			                 "`");
	}
	else
	{
		if (specs.is_inline)
			throw ParseError("inline on variable `" + name->name + "`");
		// 7.1.5p9: constexpr declares the object const.
		if (specs.is_constexpr)
			type = MakeCvQualifiedType(type, true, false);
	}
	if (name->Qualified())
		return DeclareQualified(specs, *name, type);
	if (type->kind == TK_FUNCTION)
		return DeclareFunction(specs, name->name, type);
	return DeclareVariable(specs, name->name, type);
}

DeclaredEntity* DeclParser::DeclareQualified(const DeclSpecifiers& specs,
                                             const QualifiedName& name,
                                             const TypePtr& type)
{
	// 7.3.1.2p2: redeclares a member already declared in the nominated
	// namespace; never a first declaration.
	const Binding* found = MemberLookup(*name.scope, name.name);
	if (!found || found->imported)
		throw ParseError("`" + name.name +
		                 "` is not a member of the nominated namespace");
	if (type->kind == TK_FUNCTION)
	{
		if (found->kind != BK_FUNCTION)
			throw ParseError("`" + name.name +
			                 "` redeclared as a different kind of entity");
		for (size_t i = 0; i < found->overloads.size(); i++)
		{
			DeclaredEntity* entity = found->overloads[i].entity;
			if (!SameSignature(entity->type, type))
				continue;
			if (found->overloads[i].imported)
				throw ParseError("`" + name.name + "` does not declare a "
				                 "member of the nominated namespace");
			if (!TypeEquals(entity->type, type))
				throw ParseError("`" + name.name +
				                 "` redeclared with a different return "
				                 "type");
			CheckRedeclarationSpecifiers(entity, specs);
			entity->is_inline = entity->is_inline || specs.is_inline;
			return entity;
		}
		throw ParseError("qualified declaration of `" + name.name +
		                 "` matches no declared overload");
	}
	if (found->kind != BK_VARIABLE)
		throw ParseError("`" + name.name +
		                 "` redeclared as a different kind of entity");
	DeclaredEntity* entity = found->entity;
	entity->type = MergeRedeclaredType(entity->type, type);
	CheckRedeclarationSpecifiers(entity, specs);
	entity->is_constexpr = entity->is_constexpr || specs.is_constexpr;
	return entity;
}

DeclaredEntity* DeclParser::DeclareVariable(const DeclSpecifiers& specs,
                                            const string& name,
                                            const TypePtr& type)
{
	Namespace& current = *scopes_.back();
	map<string, Binding>::iterator it = current.bindings.find(name);
	if (it != current.bindings.end())
	{
		Binding& binding = it->second;
		if (binding.kind != BK_VARIABLE)
			throw ParseError("`" + name +
			                 "` redeclared as a different kind of entity");
		if (binding.imported)
			throw ParseError("`" + name +
			                 "` conflicts with a using-declaration");
		DeclaredEntity* entity = binding.entity;
		entity->type = MergeRedeclaredType(entity->type, type);
		CheckRedeclarationSpecifiers(entity, specs);
		entity->is_constexpr = entity->is_constexpr || specs.is_constexpr;
		return entity;
	}
	DeclaredEntity* entity = LinkNewEntity(name, type, SLOT_VARIABLE, specs);
	current.variables.push_back(entity);
	Binding binding;
	binding.kind = BK_VARIABLE;
	binding.entity = entity;
	current.bindings[name] = binding;
	return entity;
}

DeclaredEntity* DeclParser::DeclareFunction(const DeclSpecifiers& specs,
                                            const string& name,
                                            const TypePtr& type)
{
	Namespace& current = *scopes_.back();
	map<string, Binding>::iterator it = current.bindings.find(name);
	if (it != current.bindings.end() && it->second.kind != BK_FUNCTION)
		throw ParseError("`" + name +
		                 "` redeclared as a different kind of entity");
	if (it != current.bindings.end())
	{
		Binding& binding = it->second;
		for (size_t i = 0; i < binding.overloads.size(); i++)
		{
			DeclaredEntity* entity = binding.overloads[i].entity;
			if (!SameSignature(entity->type, type))
				continue;
			if (binding.overloads[i].imported)
				throw ParseError("`" + name +
				                 "` conflicts with a using-declaration");
			if (!TypeEquals(entity->type, type))
				throw ParseError("`" + name + "` redeclared with a "
				                 "different return type");
			CheckRedeclarationSpecifiers(entity, specs);
			entity->is_inline = entity->is_inline || specs.is_inline;
			return entity;
		}
		// A new overload of a name this namespace already binds.
		DeclaredEntity* entity =
			LinkNewEntity(name, type, SLOT_FUNCTION, specs);
		current.functions.push_back(entity);
		binding.overloads.push_back(FunctionOverload(entity, false));
		return entity;
	}
	DeclaredEntity* entity = LinkNewEntity(name, type, SLOT_FUNCTION, specs);
	current.functions.push_back(entity);
	Binding binding;
	binding.kind = BK_FUNCTION;
	binding.overloads.push_back(FunctionOverload(entity, false));
	current.bindings[name] = binding;
	return entity;
}

// First declaration of a name in the current namespace: computes the
// linkage (3.5p3-4), links against another translation unit's entity
// when external, and reconciles the declarations when linked.
DeclaredEntity* DeclParser::LinkNewEntity(const string& name,
                                          const TypePtr& type,
                                          ESlotKind entity_kind,
                                          const DeclSpecifiers& specs)
{
	Namespace& current = *scopes_.back();
	ELinkage linkage = LNK_EXTERNAL;
	if (specs.is_static || current.link_id < 0)  // 3.5p4: unnamed context
		linkage = LNK_INTERNAL;
	else if (entity_kind == SLOT_VARIABLE && !specs.is_extern)
	{
		bool is_const, is_volatile;
		TopCv(type, is_const, is_volatile);
		if (is_const)
			linkage = LNK_INTERNAL;
	}
	string key;
	if (linkage == LNK_EXTERNAL)
	{
		key = to_string(current.link_id) + ":" + name;
		if (entity_kind == SLOT_FUNCTION)
			key += SignatureKey(type);
	}
	bool created;
	DeclaredEntity* entity = program_.LinkEntity(key, entity_kind, name,
	                                             type, linkage, created);
	if (created)
	{
		entity->is_constexpr = specs.is_constexpr;
		entity->is_inline = specs.is_inline;
		entity->is_thread_local = specs.is_thread_local;
		return entity;
	}
	// Linked to a previous translation unit's declarations (3.5p9-10).
	if (entity_kind == SLOT_FUNCTION && !TypeEquals(entity->type, type))
		throw ParseError("`" + name +
		                 "` redeclared with a different return type");
	entity->type = MergeRedeclaredType(entity->type, type);
	CheckRedeclarationSpecifiers(entity, specs);
	entity->is_inline = entity->is_inline || specs.is_inline;
	entity->is_constexpr = entity->is_constexpr || specs.is_constexpr;
	return entity;
}

void DeclParser::CheckRedeclarationSpecifiers(
	DeclaredEntity* entity, const DeclSpecifiers& specs) const
{
	// 7.1.1p7 / 3.5p6: static after a declaration with external linkage.
	if (specs.is_static && entity->linkage == LNK_EXTERNAL)
		throw ParseError("static declaration of `" + entity->name +
		                 "` follows non-static declaration");
	// 7.1.5p2: every declaration of a constexpr function is constexpr.
	if (entity->kind == SLOT_FUNCTION &&
	    entity->is_constexpr != specs.is_constexpr)
		throw ParseError("constexpr disagreement on redeclaration of `" +
		                 entity->name + "`");
	// 7.1.1: thread_local appears on every declaration of the variable
	// or on none (real linkers enforce this across translation units).
	if (entity->kind == SLOT_VARIABLE &&
	    entity->is_thread_local != specs.is_thread_local)
		throw ParseError("thread_local disagreement on redeclaration of `" +
		                 entity->name + "`");
}

void DeclParser::BindTypedef(const string& name, const TypePtr& type)
{
	Namespace& current = *scopes_.back();
	map<string, Binding>::iterator it = current.bindings.find(name);
	if (it != current.bindings.end())
	{
		// 7.1.3p3: a typedef may redefine a name to the type it already
		// refers to; anything else conflicts.
		if (it->second.kind != BK_TYPEDEF ||
		    !TypeEquals(it->second.type, type))
			throw ParseError("typedef `" + name +
			                 "` conflicts with a previous declaration");
		return;
	}
	Binding binding;
	binding.kind = BK_TYPEDEF;
	binding.type = type;
	current.bindings[name] = binding;
}

// --- expressions ---

Expr DeclParser::ParseExpression()
{
	if (AtSimple(KW_TRUE))
	{
		Advance();
		return MakeBoolLiteralExpr(true);
	}
	if (AtSimple(KW_FALSE))
	{
		Advance();
		return MakeBoolLiteralExpr(false);
	}
	if (AtSimple(KW_NULLPTR))
	{
		Advance();
		return MakeNullptrLiteralExpr();
	}
	if (AtSimple(OP_LPAREN))
	{
		Advance();
		Expr inner = ParseExpression();
		ExpectSimple(OP_RPAREN);
		return inner;
	}
	const PostToken& token = Peek();
	if (token.kind == PTK_LITERAL)
	{
		Expr literal = MakeLiteralExpr(token);
		Advance();
		return literal;
	}
	if (token.kind == PTK_LITERAL_ARRAY)
	{
		// Each string-literal token is its own image object (Block 3,
		// token order = parse order).
		ImageSlot* slot = program_.CreateStringLiteral(token);
		Advance();
		return MakeStringLiteralExpr(slot);
	}
	if (AtIdentifier() || AtSimple(OP_COLON2))
		return AnnotateIdExpression(ParseIdExpression());
	throw ParseError("expected expression");
}

Expr DeclParser::AnnotateIdExpression(const QualifiedName& name)
{
	Binding found;
	if (!ResolveComponents(name.root_global, name.path, name.name,
	                       LF_ANY_ENTITY, found))
		throw ParseError("`" + name.name +
		                 "` was not declared in this scope");
	if (found.kind == BK_VARIABLE)
		return MakeVariableExpr(found.entity, program_.current_tu());
	if (found.kind == BK_FUNCTION)
	{
		vector<DeclaredEntity*> set;
		for (size_t i = 0; i < found.overloads.size(); i++)
			set.push_back(found.overloads[i].entity);
		return MakeFunctionExpr(set);
	}
	throw ParseError("`" + name.name +
	                 "` does not name a variable or function");
}

// --- names ---

DeclParser::QualifiedName DeclParser::ParseIdExpression()
{
	QualifiedName name;
	if (AtSimple(OP_COLON2))
	{
		name.root_global = true;
		Advance();
	}
	while (AtIdentifier() && AtSimple(OP_COLON2, 1))
	{
		name.path.push_back(Peek().source);
		Advance();
		Advance();
	}
	name.name = ExpectIdentifier();
	return name;
}

Namespace* DeclParser::ParseNamespaceSpecifier()
{
	QualifiedName name = ParseIdExpression();
	Binding found;
	if (!ResolveComponents(name.root_global, name.path, name.name,
	                       LF_NAMESPACES_ONLY, found))
		throw ParseError("unknown namespace `" + name.name + "`");
	return found.target;
}

TypePtr DeclParser::ResolveTypeName(const QualifiedName& name) const
{
	Binding found;
	if (!ResolveComponents(name.root_global, name.path, name.name,
	                       LF_ANY_ENTITY, found) ||
	    found.kind != BK_TYPEDEF)
		throw ParseError("`" + name.name + "` does not name a type");
	return found.type;
}

Namespace* DeclParser::ResolveNamespacePath(
	bool root_global, const vector<string>& path) const
{
	Namespace* ns;
	size_t start = 0;
	if (root_global)
		ns = model_.global();
	else
	{
		Binding found;
		if (path.empty() ||
		    !UnqualifiedLookup(scopes_, path[0], LF_NAMESPACES_ONLY, found,
		                       &lookup_cache_))
			return 0;
		ns = found.target;
		start = 1;
	}
	for (size_t i = start; i < path.size(); i++)
	{
		Binding found;
		if (!QualifiedLookup(*ns, path[i], LF_NAMESPACES_ONLY, found))
			return 0;
		ns = found.target;
	}
	return ns;
}

bool DeclParser::ResolveComponents(bool root_global,
                                   const vector<string>& path,
                                   const string& last,
                                   ELookupFilter filter, Binding& out) const
{
	if (!root_global && path.empty())
		return UnqualifiedLookup(scopes_, last, filter, out,
		                         &lookup_cache_);
	Namespace* ns = ResolveNamespacePath(root_global, path);
	if (!ns)
		return false;
	return QualifiedLookup(*ns, last, filter, out);
}

// --- specifiers ---

DeclParser::DeclSpecifiers
DeclParser::ParseDeclSpecifierSeq(ESpecifierContext context)
{
	SpecifierState state;
	while (true)
	{
		const PostToken& token = Peek();
		if (token.kind == PTK_SIMPLE && token.token_type != OP_COLON2)
		{
			if (!ConsumeSpecifierKeyword(state, context))
				break;
			continue;
		}
		// An identifier (or :: starting a qualified name) is a type-name
		// specifier only while no type has been seen; afterwards it
		// begins the declarator.
		if ((token.kind == PTK_IDENTIFIER ||
		     (token.kind == PTK_SIMPLE && token.token_type == OP_COLON2)) &&
		    !SeenType(state))
		{
			state.named = ResolveTypeName(ParseIdExpression());
			continue;
		}
		break;
	}
	if (state.is_static && state.is_extern)
		throw ParseError("multiple storage class specifiers");
	if (state.is_typedef &&
	    (state.is_static || state.is_extern || state.is_thread_local ||
	     state.is_constexpr || state.is_inline))
		throw ParseError("typedef combined with another specifier");
	DeclSpecifiers specs;
	specs.is_typedef = state.is_typedef;
	specs.is_static = state.is_static;
	specs.is_thread_local = state.is_thread_local;
	specs.is_extern = state.is_extern;
	specs.is_constexpr = state.is_constexpr;
	specs.is_inline = state.is_inline;
	if (state.named)
	{
		if (state.has_base || state.signed_count || state.unsigned_count ||
		    state.short_count || state.long_count)
			throw ParseError("type-name combined with simple type "
			                 "specifiers");
		specs.type = MakeCvQualifiedType(state.named, state.is_const,
		                                 state.is_volatile);
	}
	else
		specs.type = MakeCvQualifiedType(
			MakeFundamentalType(CombineFundamental(state)), state.is_const,
			state.is_volatile);
	return specs;
}

bool DeclParser::ConsumeSpecifierKeyword(SpecifierState& state,
                                         ESpecifierContext context)
{
	switch (Peek().token_type)
	{
	case KW_STATIC:
	case KW_THREAD_LOCAL:
	case KW_EXTERN:
	case KW_TYPEDEF:
	case KW_CONSTEXPR:
	case KW_INLINE:
	{
		// 8.3.5p2 (parameters) / 8.4 type-ids: only type-specifiers.
		if (context != SC_DECLARATION)
			throw ParseError("specifier not allowed in this context");
		bool* flag;
		switch (Peek().token_type)
		{
		case KW_STATIC: flag = &state.is_static; break;
		case KW_THREAD_LOCAL: flag = &state.is_thread_local; break;
		case KW_EXTERN: flag = &state.is_extern; break;
		case KW_TYPEDEF: flag = &state.is_typedef; break;
		case KW_CONSTEXPR: flag = &state.is_constexpr; break;
		default: flag = &state.is_inline; break;
		}
		// 7.1.1p1 (storage classes) and the per-specifier rules: each of
		// these may appear at most once in a decl-specifier-seq.
		if (*flag)
			throw ParseError("duplicate specifier");
		*flag = true;
		break;
	}
	case KW_CONST:
		// 7.1.6.2p2: redundant cv-qualifiers are ill-formed except when
		// introduced through a typedef-name.
		if (state.is_const)
			throw ParseError("duplicate const");
		state.is_const = true;
		break;
	case KW_VOLATILE:
		if (state.is_volatile)
			throw ParseError("duplicate volatile");
		state.is_volatile = true;
		break;
	case KW_SIGNED:
		state.signed_count++;
		break;
	case KW_UNSIGNED:
		state.unsigned_count++;
		break;
	case KW_SHORT:
		state.short_count++;
		break;
	case KW_LONG:
		state.long_count++;
		break;
	case KW_CHAR:
	case KW_CHAR16_T:
	case KW_CHAR32_T:
	case KW_WCHAR_T:
	case KW_BOOL:
	case KW_INT:
	case KW_FLOAT:
	case KW_DOUBLE:
	case KW_VOID:
		if (state.has_base || state.named)
			throw ParseError("multiple type specifiers");
		state.has_base = true;
		state.base = Peek().token_type;
		break;
	default:
		return false;
	}
	Advance();
	return true;
}

bool DeclParser::SeenType(const SpecifierState& state)
{
	return state.has_base || state.named || state.signed_count > 0 ||
		state.unsigned_count > 0 || state.short_count > 0 ||
		state.long_count > 0;
}

// The 7.1.6.2p3 simple-type-specifier combination table.
EFundamentalType
DeclParser::CombineFundamental(const SpecifierState& state) const
{
	if (!SeenType(state))
		throw ParseError("declaration requires a type specifier");
	bool is_unsigned = state.unsigned_count > 0;
	if (state.signed_count + state.unsigned_count > 1 ||
	    state.short_count > 1 || state.long_count > 2 ||
	    (state.short_count && state.long_count))
		throw ParseError("invalid type specifier combination");
	bool modified = state.signed_count || state.unsigned_count ||
		state.short_count || state.long_count;
	switch (state.has_base ? state.base : KW_INT)
	{
	case KW_CHAR:
		if (state.short_count || state.long_count)
			throw ParseError("invalid type specifier combination");
		if (is_unsigned)
			return FT_UNSIGNED_CHAR;
		return state.signed_count ? FT_SIGNED_CHAR : FT_CHAR;
	case KW_CHAR16_T:
	case KW_CHAR32_T:
	case KW_WCHAR_T:
	case KW_BOOL:
	case KW_FLOAT:
	case KW_VOID:
		if (modified)
			throw ParseError("invalid type specifier combination");
		switch (state.base)
		{
		case KW_CHAR16_T: return FT_CHAR16_T;
		case KW_CHAR32_T: return FT_CHAR32_T;
		case KW_WCHAR_T: return FT_WCHAR_T;
		case KW_BOOL: return FT_BOOL;
		case KW_FLOAT: return FT_FLOAT;
		default: return FT_VOID;
		}
	case KW_DOUBLE:
		if (state.signed_count || state.unsigned_count ||
		    state.short_count || state.long_count > 1)
			throw ParseError("invalid type specifier combination");
		return state.long_count ? FT_LONG_DOUBLE : FT_DOUBLE;
	default:  // KW_INT, spelled or implied
		if (state.short_count)
			return is_unsigned ? FT_UNSIGNED_SHORT_INT : FT_SHORT_INT;
		if (state.long_count == 2)
			return is_unsigned ? FT_UNSIGNED_LONG_LONG_INT
			                   : FT_LONG_LONG_INT;
		if (state.long_count == 1)
			return is_unsigned ? FT_UNSIGNED_LONG_INT : FT_LONG_INT;
		return is_unsigned ? FT_UNSIGNED_INT : FT_INT;
	}
}

TypePtr DeclParser::ParseTypeId()
{
	DeclSpecifiers specs = ParseDeclSpecifierSeq(SC_TYPE_ID);
	Declarator declarator;
	ParseDeclarator(DM_ABSTRACT, declarator);
	return ComputeDeclaratorType(specs.type, declarator);
}

// --- declarators ---

void DeclParser::ParseDeclarator(EDeclaratorMode mode, Declarator& out)
{
	ParsePtrOperators(out.prefix);
	ParseDeclaratorRoot(mode, out);
	ParseDeclaratorSuffixes(out.suffixes);
}

void DeclParser::ParsePtrOperators(vector<DeclaratorChunk>& out)
{
	while (true)
	{
		DeclaratorChunk chunk;
		if (AtSimple(OP_STAR))
		{
			Advance();
			chunk.kind = DeclaratorChunk::CK_POINTER;
			while (AtSimple(KW_CONST) || AtSimple(KW_VOLATILE))
			{
				// 7.1.6.1p1: at most one of each in a cv-qualifier-seq.
				if (AtSimple(KW_CONST))
				{
					if (chunk.is_const)
						throw ParseError("duplicate const");
					chunk.is_const = true;
				}
				else
				{
					if (chunk.is_volatile)
						throw ParseError("duplicate volatile");
					chunk.is_volatile = true;
				}
				Advance();
			}
		}
		else if (AtSimple(OP_AMP))
		{
			Advance();
			chunk.kind = DeclaratorChunk::CK_LVALUE_REF;
		}
		else if (AtSimple(OP_LAND))
		{
			Advance();
			chunk.kind = DeclaratorChunk::CK_RVALUE_REF;
		}
		else
			return;
		out.push_back(chunk);
	}
}

void DeclParser::ParseDeclaratorRoot(EDeclaratorMode mode, Declarator& out)
{
	if (AtIdentifier() || AtSimple(OP_COLON2))
	{
		if (mode == DM_ABSTRACT)
			throw ParseError("unexpected declarator-id in abstract "
			                 "declarator");
		out.name = ParseIdExpression();
		out.has_name = true;
		if (out.name.Qualified())
		{
			if (mode != DM_NAMED)
				throw ParseError("parameter name shall not be qualified");
			EnterQualifiedDeclaratorScope(out.name);
		}
		return;
	}
	if (AtSimple(OP_LPAREN))
	{
		// A named declarator cannot be a bare parameter clause; for the
		// abstract-capable modes 8.2p7 prefers the parameter clause
		// reading, which then continues in the suffix loop.
		if (mode != DM_NAMED && LParenStartsParameters())
			return;
		Advance();
		out.inner.reset(new Declarator());
		ParseDeclarator(mode, *out.inner);
		ExpectSimple(OP_RPAREN);
		return;
	}
	if (mode == DM_NAMED)
		throw ParseError("expected declarator-id");
}

void DeclParser::EnterQualifiedDeclaratorScope(QualifiedName& name)
{
	Namespace* target = ResolveNamespacePath(name.root_global, name.path);
	if (!target)
		throw ParseError("unknown namespace in qualified declarator `" +
		                 name.name + "`");
	// 8.3p1 / 7.3.1.2p2: the declaration must appear in a namespace
	// enclosing the member's namespace.
	if (!NamespaceEncloses(scopes_.back(), target))
		throw ParseError("qualified declarator `" + name.name +
		                 "` outside an enclosing namespace");
	name.scope = target;
	// 3.4.3p3: names after the qualified declarator-id (array bounds,
	// the initializer) are looked up in the member's namespace until
	// RestoreDeclaratorScope. At most one declarator-id exists per
	// init-declarator (parameters reject qualified names), so the one
	// save slot cannot be clobbered.
	saved_scopes_.swap(scopes_);
	scopes_ = NamespaceChain(target);
	scope_switched_ = true;
}

void DeclParser::RestoreDeclaratorScope()
{
	if (!scope_switched_)
		return;
	scopes_.swap(saved_scopes_);
	saved_scopes_.clear();
	scope_switched_ = false;
}

void DeclParser::ParseDeclaratorSuffixes(vector<DeclaratorChunk>& out)
{
	while (true)
	{
		if (AtSimple(OP_LPAREN))
			out.push_back(ParseParametersAndQualifiers());
		else if (AtSimple(OP_LSQUARE))
			out.push_back(ParseArrayBound());
		else
			return;
	}
}

DeclParser::DeclaratorChunk DeclParser::ParseParametersAndQualifiers()
{
	DeclaratorChunk chunk;
	chunk.kind = DeclaratorChunk::CK_FUNCTION;
	ExpectSimple(OP_LPAREN);
	if (AtSimple(OP_DOTS))
	{
		chunk.variadic = true;
		Advance();
	}
	else if (!AtSimple(OP_RPAREN))
	{
		chunk.parameters.push_back(ParseParameterDeclaration());
		while (!chunk.variadic)
		{
			if (AtSimple(OP_DOTS))  // parameter-declaration-list ...
			{
				chunk.variadic = true;
				Advance();
				break;
			}
			if (!AtSimple(OP_COMMA))
				break;
			Advance();
			if (AtSimple(OP_DOTS))  // parameter-declaration-list , ...
			{
				chunk.variadic = true;
				Advance();
				break;
			}
			chunk.parameters.push_back(ParseParameterDeclaration());
		}
	}
	ExpectSimple(OP_RPAREN);
	// 8.3.5p4: the parameter list (void) is the empty parameter list.
	if (!chunk.variadic && chunk.parameters.size() == 1 &&
	    IsUnqualifiedVoid(chunk.parameters[0]))
		chunk.parameters.clear();
	return chunk;
}

TypePtr DeclParser::ParseParameterDeclaration()
{
	DeclSpecifiers specs = ParseDeclSpecifierSeq(SC_PARAMETER);
	Declarator declarator;
	ParseDeclarator(DM_PARAMETER, declarator);
	return AdjustParameterType(ComputeDeclaratorType(specs.type,
	                                                 declarator));
}

DeclParser::DeclaratorChunk DeclParser::ParseArrayBound()
{
	DeclaratorChunk chunk;
	chunk.kind = DeclaratorChunk::CK_ARRAY;
	ExpectSimple(OP_LSQUARE);
	if (!AtSimple(OP_RSQUARE))
	{
		Expr bound = ParseExpression();
		chunk.bound = EvaluateArrayBound(bound, program_.current_tu());
		chunk.bound_known = true;
	}
	ExpectSimple(OP_RSQUARE);
	return chunk;
}

bool DeclParser::LParenStartsParameters() const
{
	const PostToken& next = Peek(1);
	if (next.kind == PTK_IDENTIFIER)
		return ScanIsTypeName(1);
	if (next.kind != PTK_SIMPLE)
		return false;
	switch (next.token_type)
	{
	case OP_RPAREN:
	case OP_DOTS:
		return true;
	case OP_COLON2:
		return ScanIsTypeName(1);
	case OP_STAR:
	case OP_AMP:
	case OP_LAND:
	case OP_LPAREN:
		return false;
	default:
		return IsDeclSpecifierKeyword(next.token_type);
	}
}

// Positional scan of an optionally qualified name starting at `ahead`;
// true when it resolves to a typedef-name. Lookup never modifies the
// model, so the scan has no side effects.
bool DeclParser::ScanIsTypeName(size_t ahead) const
{
	bool root_global = false;
	vector<string> path;
	if (AtSimple(OP_COLON2, ahead))
	{
		root_global = true;
		ahead++;
	}
	while (AtIdentifier(ahead) && AtSimple(OP_COLON2, ahead + 1))
	{
		path.push_back(Peek(ahead).source);
		ahead += 2;
	}
	if (!AtIdentifier(ahead))
		return false;
	Binding found;
	return ResolveComponents(root_global, path, Peek(ahead).source,
	                         LF_ANY_ENTITY, found) &&
		found.kind == BK_TYPEDEF;
}

TypePtr DeclParser::ComputeDeclaratorType(const TypePtr& base,
                                          const Declarator& declarator) const
{
	return ComputeDeclaratorTypeRec(base, declarator, base);
}

TypePtr DeclParser::ComputeDeclaratorTypeRec(TypePtr type,
                                             const Declarator& declarator,
                                             const TypePtr& spec_base) const
{
	for (size_t i = 0; i < declarator.prefix.size(); i++)
	{
		const DeclaratorChunk& chunk = declarator.prefix[i];
		if (chunk.kind == DeclaratorChunk::CK_POINTER)
			type = MakePointerType(type, chunk.is_const, chunk.is_volatile);
		else
			// 8.3.2p6: a reference applied to a reference collapses only
			// when the inner one came through the typedef-name in the
			// decl-specifier-seq; a chunk-built reference is a direct,
			// ill-formed reference-to-reference.
			type = MakeReferenceType(
				type, chunk.kind == DeclaratorChunk::CK_RVALUE_REF,
				type.get() == spec_base.get());
	}
	for (size_t i = declarator.suffixes.size(); i-- > 0;)
	{
		const DeclaratorChunk& chunk = declarator.suffixes[i];
		if (chunk.kind == DeclaratorChunk::CK_FUNCTION)
			type = MakeFunctionType(type, chunk.parameters, chunk.variadic);
		else
			type = MakeArrayType(type, chunk.bound_known, chunk.bound);
	}
	if (declarator.inner)
		return ComputeDeclaratorTypeRec(type, *declarator.inner, spec_base);
	return type;
}

const DeclParser::QualifiedName*
DeclParser::DeclaratorName(const Declarator& declarator)
{
	if (declarator.has_name)
		return &declarator.name;
	if (declarator.inner)
		return DeclaratorName(*declarator.inner);
	return 0;
}

bool DeclParser::HasDeclaratorChunks(const Declarator& declarator)
{
	if (!declarator.prefix.empty() || !declarator.suffixes.empty())
		return true;
	return declarator.inner && HasDeclaratorChunks(*declarator.inner);
}
