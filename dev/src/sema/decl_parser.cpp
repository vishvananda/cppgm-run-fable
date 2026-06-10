#include "sema/decl_parser.h"

using std::runtime_error;

namespace {

bool IsIntegralLiteralType(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_INT:
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_CHAR:
	case FT_UNSIGNED_SHORT_INT:
	case FT_UNSIGNED_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
	case FT_WCHAR_T:
	case FT_CHAR:
	case FT_CHAR16_T:
	case FT_CHAR32_T:
	case FT_BOOL:
		return true;
	default:
		return false;
	}
}

bool IsSignedLiteralType(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_INT:
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_CHAR:     // signed on the x86-64 ABI
	case FT_WCHAR_T:  // int on the x86-64 ABI
		return true;
	default:
		return false;
	}
}

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
	return type->kind == TK_FUNDAMENTAL && type->fundamental == FT_VOID &&
		!type->is_const && !type->is_volatile;
}

} // namespace

DeclParser::DeclParser(const vector<PostToken>& tokens, SemaModel& model)
	: tokens_(tokens), pos_(0), model_(model)
{
	eof_.kind = PTK_EOF;
	for (size_t i = 0; i < tokens_.size(); i++)
		if (tokens_[i].kind == PTK_INVALID)
			throw runtime_error("invalid token at phase 7: " +
			                    tokens_[i].source);
}

void DeclParser::ParseTranslationUnit()
{
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
	return runtime_error("pa7 parse error: " + message + " at " + at);
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
	if (AtSimple(KW_INLINE))
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
		if (it != parent.bindings.end() && it->second.kind == BK_NAMESPACE)
			ns = it->second.target;
	}
	if (!ns)
	{
		ns = AddMemberNamespace(model_, parent, name, is_inline);
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
	Binding binding;
	binding.kind = BK_NAMESPACE;
	binding.target = target;
	scopes_.back()->bindings[name] = binding;
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
	const Binding* found = ResolveComponents(name.root_global, name.path,
	                                         name.name, LF_ANY_ENTITY);
	if (!found)
		throw ParseError("using-declaration names unknown `" + name.name +
		                 "`");
	if (found->kind == BK_NAMESPACE)
		throw ParseError("using-declaration shall not name a namespace");
	// The current namespace re-binds the same entity or alias; the
	// entity still prints only under its first namespace.
	scopes_.back()->bindings[name.name] = *found;
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

void DeclParser::ParseSimpleDeclaration()
{
	DeclSpecifiers specs = ParseDeclSpecifierSeq(true);
	while (true)
	{
		Declarator declarator;
		ParseDeclarator(DM_NAMED, declarator);
		DeclareSimple(specs, declarator);
		if (!AtSimple(OP_COMMA))
			break;
		Advance();
	}
	ExpectSimple(OP_SEMICOLON);
}

void DeclParser::DeclareSimple(const DeclSpecifiers& specs,
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
		return;
	}
	if (name->Qualified())
	{
		// 7.3.1.2p2: redeclares a member already declared in the
		// nominated namespace; never a first declaration.
		const Binding* found = ResolveComponents(
			name->root_global, name->path, name->name, LF_ANY_ENTITY);
		if (!found ||
		    (found->kind != BK_VARIABLE && found->kind != BK_FUNCTION))
			throw ParseError("qualified declarator-id `" + name->name +
			                 "` does not redeclare a namespace member");
		found->entity->type = MergeRedeclaredType(found->entity->type, type);
		return;
	}
	Namespace& current = *scopes_.back();
	map<string, Binding>::iterator it = current.bindings.find(name->name);
	if (it != current.bindings.end() &&
	    (it->second.kind == BK_VARIABLE || it->second.kind == BK_FUNCTION))
	{
		it->second.entity->type =
			MergeRedeclaredType(it->second.entity->type, type);
		return;
	}
	DeclaredEntity* entity = model_.CreateEntity(name->name, type);
	bool is_function = type->kind == TK_FUNCTION;
	if (is_function)
		current.functions.push_back(entity);
	else
		current.variables.push_back(entity);
	Binding binding;
	binding.kind = is_function ? BK_FUNCTION : BK_VARIABLE;
	binding.entity = entity;
	current.bindings[name->name] = binding;
}

void DeclParser::BindTypedef(const string& name, const TypePtr& type)
{
	Binding binding;
	binding.kind = BK_TYPEDEF;
	binding.type = type;
	scopes_.back()->bindings[name] = binding;
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
	const Binding* found = ResolveComponents(name.root_global, name.path,
	                                         name.name, LF_NAMESPACES_ONLY);
	if (!found)
		throw ParseError("unknown namespace `" + name.name + "`");
	return found->target;
}

TypePtr DeclParser::ResolveTypeName(const QualifiedName& name) const
{
	const Binding* found = ResolveComponents(name.root_global, name.path,
	                                         name.name, LF_ANY_ENTITY);
	if (!found || found->kind != BK_TYPEDEF)
		throw ParseError("`" + name.name + "` does not name a type");
	return found->type;
}

const Binding* DeclParser::ResolveComponents(bool root_global,
                                             const vector<string>& path,
                                             const string& last,
                                             ELookupFilter filter) const
{
	if (!root_global && path.empty())
		return UnqualifiedLookup(scopes_, last, filter, &lookup_cache_);
	Namespace* ns;
	size_t start = 0;
	if (root_global)
		ns = model_.global();
	else
	{
		const Binding* found =
			UnqualifiedLookup(scopes_, path[0], LF_NAMESPACES_ONLY,
			                  &lookup_cache_);
		if (!found)
			return 0;
		ns = found->target;
		start = 1;
	}
	for (size_t i = start; i < path.size(); i++)
	{
		const Binding* found =
			QualifiedLookup(*ns, path[i], LF_NAMESPACES_ONLY);
		if (!found)
			return 0;
		ns = found->target;
	}
	return QualifiedLookup(*ns, last, filter);
}

// --- specifiers ---

DeclParser::DeclSpecifiers
DeclParser::ParseDeclSpecifierSeq(bool allow_decl_specifiers)
{
	SpecifierState state;
	while (true)
	{
		const PostToken& token = Peek();
		if (token.kind == PTK_SIMPLE && token.token_type != OP_COLON2)
		{
			if (!ConsumeSpecifierKeyword(state, allow_decl_specifiers))
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
	DeclSpecifiers specs;
	specs.is_typedef = state.is_typedef;
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
                                         bool allow_decl_specifiers)
{
	switch (Peek().token_type)
	{
	case KW_STATIC:
	case KW_THREAD_LOCAL:
	case KW_EXTERN:
		// Storage class never affects the PA7 type or description.
		if (!allow_decl_specifiers)
			return false;
		break;
	case KW_TYPEDEF:
		if (!allow_decl_specifiers)
			return false;
		state.is_typedef = true;
		break;
	case KW_CONST:
		state.is_const = true;
		break;
	case KW_VOLATILE:
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
	DeclSpecifiers specs = ParseDeclSpecifierSeq(false);
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
				if (AtSimple(KW_CONST))
					chunk.is_const = true;
				else
					chunk.is_volatile = true;
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
	DeclSpecifiers specs = ParseDeclSpecifierSeq(true);
	if (specs.is_typedef)
		throw ParseError("typedef in parameter declaration");
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
		chunk.bound = EvaluateArrayBound(Peek());
		chunk.bound_known = true;
		Advance();
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
	const Binding* found = ResolveComponents(root_global, path,
	                                         Peek(ahead).source,
	                                         LF_ANY_ENTITY);
	return found && found->kind == BK_TYPEDEF;
}

TypePtr DeclParser::ComputeDeclaratorType(TypePtr base,
                                          const Declarator& declarator) const
{
	TypePtr type = base;
	for (size_t i = 0; i < declarator.prefix.size(); i++)
	{
		const DeclaratorChunk& chunk = declarator.prefix[i];
		if (chunk.kind == DeclaratorChunk::CK_POINTER)
			type = MakePointerType(type, chunk.is_const, chunk.is_volatile);
		else
			type = MakeReferenceType(
				type, chunk.kind == DeclaratorChunk::CK_RVALUE_REF);
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
		return ComputeDeclaratorType(type, *declarator.inner);
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

// 8.3.4p1 via the PA7 handout: the bound is a converted constant
// expression of type size_t with a value greater than zero, and the
// pa7.gram constant-expression is one non-user-defined literal.
unsigned long long
DeclParser::EvaluateArrayBound(const PostToken& literal) const
{
	if (literal.kind != PTK_LITERAL ||
	    !IsIntegralLiteralType(literal.type))
		throw ParseError("array bound must be an integral literal");
	unsigned long long value = LittleEndianValue(literal.data);
	if (IsSignedLiteralType(literal.type) && !literal.data.empty() &&
	    (literal.data[literal.data.size() - 1] & 0x80))
		throw ParseError("array bound must be positive");
	if (value == 0)
		throw ParseError("array bound must be greater than zero");
	return value;
}
