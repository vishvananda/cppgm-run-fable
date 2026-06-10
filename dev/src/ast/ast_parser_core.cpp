#include "ast/ast_parser.h"

using std::string;
using std::vector;

AstParser::AstParser(const vector<ParseToken>& tokens)
	: tokens_(tokens), pos_(0), template_decl_scope_depth_(0)
{
	table_pool_.push_back(NameTable());
	ScopeRef root;
	root.table = &table_pool_.back();
	root.param_scope = false;
	scopes_.push_back(root);
}

// --- name table --------------------------------------------------------

AstParser::NameTable* AstParser::NewTable()
{
	table_pool_.push_back(NameTable());
	return &table_pool_.back();
}

void AstParser::PushScope(NameTable* table, bool param_scope)
{
	ScopeRef scope;
	scope.table = table;
	scope.param_scope = param_scope;
	scopes_.push_back(scope);
}

void AstParser::PushTransientScope()
{
	PushScope(NewTable(), false);
}

void AstParser::PopScope()
{
	scopes_.pop_back();
}

// Registrations log their previous state so a failed parse can be
// rolled back token-exactly (Restore replays the log tail). Tables
// outlive the scope stack (table_pool_), so replay stays valid even
// for scopes popped on a success path before the enclosing Restore.
void AstParser::Register(const string& name, unsigned flags)
{
	if (name.empty())
		return;
	NameTable* table = scopes_.back().table;
	UndoEntry undo;
	undo.table = table;
	undo.name = name;
	std::map<string, unsigned>::iterator it = table->entries.find(name);
	undo.existed = it != table->entries.end();
	undo.old_flags = undo.existed ? it->second : 0;
	undo_log_.push_back(undo);
	table->entries[name] = undo.existed ? (it->second | flags) : flags;
}

// The scope a declaration's name belongs to: the nearest scope that is
// not a template-parameter scope (a template-declaration's declared
// name lives in the scope enclosing the template header).
AstParser::NameTable* AstParser::DeclScopeTable()
{
	for (size_t i = scopes_.size(); i-- > 0;)
		if (!scopes_[i].param_scope)
			return scopes_[i].table;
	return scopes_[0].table;
}

void AstParser::RegisterInDeclScope(const string& name, unsigned flags)
{
	if (name.empty())
		return;
	NameTable* table = DeclScopeTable();
	UndoEntry undo;
	undo.table = table;
	undo.name = name;
	std::map<string, unsigned>::iterator it = table->entries.find(name);
	undo.existed = it != table->entries.end();
	undo.old_flags = undo.existed ? it->second : 0;
	undo_log_.push_back(undo);
	table->entries[name] = undo.existed ? (it->second | flags) : flags;
}

AstParser::NameTable* AstParser::GetOrCreateChild(NameTable* parent,
                                                  const string& name)
{
	std::map<string, NameTable*>::iterator it = parent->children.find(name);
	if (it != parent->children.end())
		return it->second;
	NameTable* child = NewTable();
	parent->children[name] = child;
	return child;
}

unsigned AstParser::TemplatedFlag() const
{
	return scopes_.size() == template_decl_scope_depth_ ? NF_TEMPLATE : 0u;
}

int AstParser::LookupUnqualified(const string& name) const
{
	for (size_t i = scopes_.size(); i-- > 0;)
	{
		std::map<string, unsigned>::const_iterator it =
			scopes_[i].table->entries.find(name);
		if (it != scopes_[i].table->entries.end())
			return static_cast<int>(it->second);
	}
	return kUnresolved;
}

// Resolves all of a name's qualifier parts (everything in `parts`) to
// the namespace/class table they denote, descending the persistent
// child tables by identifier (template arguments ignored). Null when
// any step cannot be classified syntactically.
const AstParser::NameTable* AstParser::DescendPrefix(const AstName& prefix) const
{
	if (prefix.parts.empty())
		return 0;
	const AstNamePart& root = prefix.parts[0];
	if (root.kind != NP_IDENTIFIER && root.kind != NP_TEMPLATE_ID)
		return 0;
	const NameTable* table = 0;
	if (prefix.global_scope)
	{
		std::map<string, NameTable*>::const_iterator child =
			scopes_[0].table->children.find(root.identifier);
		if (child == scopes_[0].table->children.end())
			return 0;
		table = child->second;
	}
	else
	{
		for (size_t i = scopes_.size(); table == 0 && i-- > 0;)
		{
			std::map<string, NameTable*>::const_iterator child =
				scopes_[i].table->children.find(root.identifier);
			if (child != scopes_[i].table->children.end())
				table = child->second;
			else if (scopes_[i].table->entries.count(root.identifier))
				return 0;  // nearest declaration is a non-scope entity
		}
		if (table == 0)
			return 0;
	}
	for (size_t i = 1; i < prefix.parts.size(); i++)
	{
		const AstNamePart& part = prefix.parts[i];
		if (part.kind != NP_IDENTIFIER && part.kind != NP_TEMPLATE_ID)
			return 0;
		std::map<string, NameTable*>::const_iterator child =
			table->children.find(part.identifier);
		if (child == table->children.end())
			return 0;
		table = child->second;
	}
	return table;
}

// Classifies a structured name. Unqualified plain identifiers use the
// scope stack; qualified names descend the child tables. Any
// unresolvable step yields kUnresolved, which callers treat
// optimistically.
int AstParser::ResolveName(const AstName& name) const
{
	if (name.parts.empty())
		return kUnresolved;
	const AstNamePart& terminal = name.parts.back();
	if (terminal.kind != NP_IDENTIFIER && terminal.kind != NP_TEMPLATE_ID)
		return kUnresolved;
	if (terminal.tilde)
		return kUnresolved;
	AstName prefix;
	prefix.global_scope = name.global_scope;
	if (name.parts.size() > 1)
	{
		// Borrow the qualifier parts structurally: DescendPrefix only
		// reads kinds and identifiers.
		for (size_t i = 0; i + 1 < name.parts.size(); i++)
		{
			AstNamePart part;
			part.kind = name.parts[i].kind;
			part.identifier = name.parts[i].identifier;
			prefix.parts.push_back(std::move(part));
		}
	}
	return ResolveTerminalFlags(prefix, terminal.identifier);
}

// Type positions accept resolved types and unresolved names
// (optimistically), but never names whose nearest declaration is a
// value: a local variable shadows an outer typedef for the
// declaration-versus-expression choice.
bool AstParser::NameUsableAsType(const AstName& name) const
{
	if (name.typename_keyword)
		return true;
	int flags = ResolveName(name);
	if (flags == kUnresolved)
		return true;
	return (flags & NF_TYPE) != 0;
}

void AstParser::RegisterDeclaratorIds(const AstDecl& decl)
{
	bool is_typedef = false;
	for (size_t i = 0; i < decl.specifiers.size(); i++)
		if (decl.specifiers[i].kind == SPEC_KEYWORD &&
		    decl.specifiers[i].keyword == KW_TYPEDEF)
			is_typedef = true;
	unsigned flags = is_typedef ? NF_TYPE : NF_VALUE;
	for (size_t i = 0; i < decl.declarators.size(); i++)
	{
		const AstName* id = decl.declarators[i].declarator->IdName();
		if (id && id->IsPlainIdentifier())
			Register(id->parts[0].identifier, flags);
	}
}

// Parameters of the first parameter clause become values in the scope
// pushed for a function (or lambda) body.
void AstParser::RegisterParameters(const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		if (declarator.items[i].kind != DI_PARAMS)
			continue;
		const AstParameterClause& clause = *declarator.items[i].params;
		for (size_t p = 0; p < clause.parameters.size(); p++)
		{
			if (!clause.parameters[p].declarator)
				continue;
			const AstName* id = clause.parameters[p].declarator->IdName();
			if (id && id->IsPlainIdentifier())
				Register(id->parts[0].identifier, NF_VALUE);
		}
		return;
	}
}

// After a template-declaration parses, its declared name gains the
// template category in the enclosing scope (class and alias templates
// stay types; function templates stay values).
void AstParser::RegisterTemplateName(const AstDecl& inner)
{
	switch (inner.kind)
	{
	case DK_CLASS:
	case DK_CLASS_FORWARD:
		if (inner.has_name && inner.class_name.IsPlainIdentifier())
			RegisterInDeclScope(inner.class_name.parts[0].identifier,
			                    NF_TYPE | NF_TEMPLATE);
		break;
	case DK_ALIAS:
		RegisterInDeclScope(inner.name, NF_TYPE | NF_TEMPLATE);
		break;
	case DK_SIMPLE:
		for (size_t i = 0; i < inner.declarators.size(); i++)
		{
			const AstName* id = inner.declarators[i].declarator->IdName();
			if (id && id->IsPlainIdentifier())
				RegisterInDeclScope(id->parts[0].identifier,
				                    NF_VALUE | NF_TEMPLATE);
		}
		break;
	case DK_FUNCTION:
	{
		const AstName* id = inner.declarator->IdName();
		if (id && id->IsPlainIdentifier())
			RegisterInDeclScope(id->parts[0].identifier,
			                    NF_VALUE | NF_TEMPLATE);
		break;
	}
	case DK_TEMPLATE:
		if (inner.inner)
			RegisterTemplateName(*inner.inner);
		break;
	default:
		break;
	}
}

// --- token-stream infrastructure ---------------------------------------

AstParser::State AstParser::Save() const
{
	State state;
	state.pos = pos_;
	state.bracket_depth = brackets_.size();
	state.scope_depth = scopes_.size();
	state.undo_depth = undo_log_.size();
	return state;
}

void AstParser::Restore(const State& state)
{
	pos_ = state.pos;
	if (brackets_.size() > state.bracket_depth)
		brackets_.resize(state.bracket_depth);
	while (undo_log_.size() > state.undo_depth)
	{
		const UndoEntry& undo = undo_log_.back();
		if (undo.existed)
			undo.table->entries[undo.name] = undo.old_flags;
		else
			undo.table->entries.erase(undo.name);
		undo_log_.pop_back();
	}
	if (scopes_.size() > state.scope_depth)
		scopes_.resize(state.scope_depth);
}

const ParseToken& AstParser::Peek(size_t ahead) const
{
	size_t index = pos_ + ahead;
	if (index >= tokens_.size())
		index = tokens_.size() - 1;  // ST_EOF (BuildParseTokens guarantee)
	return tokens_[index];
}

void AstParser::Advance()
{
	const ParseToken& token = Peek();
	if (token.kind == PTOK_EOF)
		return;
	if (token.kind == PTOK_SIMPLE)
	{
		switch (token.simple_type)
		{
		case OP_LPAREN:
			brackets_.push_back('(');
			break;
		case OP_LSQUARE:
			brackets_.push_back('[');
			break;
		case OP_LBRACE:
			brackets_.push_back('{');
			break;
		case OP_RPAREN:
			if (!brackets_.empty() && brackets_.back() == '(')
				brackets_.pop_back();
			break;
		case OP_RSQUARE:
			if (!brackets_.empty() && brackets_.back() == '[')
				brackets_.pop_back();
			break;
		case OP_RBRACE:
			if (!brackets_.empty() && brackets_.back() == '{')
				brackets_.pop_back();
			break;
		default:
			break;
		}
	}
	pos_++;
}

bool AstParser::AtSimple(ETokenType type, size_t ahead) const
{
	const ParseToken& token = Peek(ahead);
	return token.kind == PTOK_SIMPLE && token.simple_type == type;
}

bool AstParser::AtIdentifier(size_t ahead) const
{
	return Peek(ahead).kind == PTOK_IDENTIFIER;
}

bool AstParser::AtIdentifierSpelled(const char* spelling) const
{
	return AtIdentifier() && Peek().spelling == spelling;
}

bool AstParser::AtLiteral() const
{
	return Peek().kind == PTOK_LITERAL;
}

bool AstParser::AtEof() const
{
	return Peek().kind == PTOK_EOF;
}

bool AstParser::MatchSimple(ETokenType type)
{
	if (!AtSimple(type))
		return false;
	Advance();
	return true;
}

bool AstParser::InAngleBrackets() const
{
	return !brackets_.empty() && brackets_.back() == '<';
}

bool AstParser::MatchOpenAngle()
{
	if (!MatchSimple(OP_LT))
		return false;
	brackets_.push_back('<');
	return true;
}

bool AstParser::AtCloseAngle() const
{
	const ParseToken& token = Peek();
	return AtSimple(OP_GT) || token.kind == PTOK_RSHIFT_1 ||
		token.kind == PTOK_RSHIFT_2;
}

// A split OP_RSHIFT closes two levels: the inner list consumes
// ST_RSHIFT_1 and the enclosing one ST_RSHIFT_2 (14.2.3).
bool AstParser::MatchCloseAngle()
{
	if (!AtCloseAngle())
		return false;
	Advance();
	if (!brackets_.empty() && brackets_.back() == '<')
		brackets_.pop_back();
	return true;
}

bool AstParser::SkipBalancedParens()
{
	if (!AtSimple(OP_LPAREN))
		return false;
	size_t depth = 0;
	do
	{
		if (AtEof())
			return false;
		if (AtSimple(OP_LPAREN))
			depth++;
		else if (AtSimple(OP_RPAREN))
			depth--;
		Advance();
	} while (depth > 0);
	return true;
}

// GNU __attribute__((...)) and alignas(...) adornments are accepted
// where specifiers can appear and discarded (the references omit
// them).
void AstParser::SkipDeclAdornments()
{
	for (;;)
	{
		State state = Save();
		if (AtIdentifierSpelled("__attribute__"))
		{
			Advance();
			if (SkipBalancedParens())
				continue;
			Restore(state);
			return;
		}
		if (AtSimple(KW_ALIGNAS))
		{
			Advance();
			if (SkipBalancedParens())
				continue;
			Restore(state);
			return;
		}
		return;
	}
}

// [[ ... ]] attribute, accepted after declarators and discarded.
bool AstParser::SkipSquareAttribute()
{
	if (!AtSimple(OP_LSQUARE) || !AtSimple(OP_LSQUARE, 1))
		return false;
	State state = Save();
	Advance();
	Advance();
	size_t depth = 2;
	while (depth > 0)
	{
		if (AtEof())
		{
			Restore(state);
			return false;
		}
		if (AtSimple(OP_LSQUARE))
			depth++;
		else if (AtSimple(OP_RSQUARE))
			depth--;
		Advance();
	}
	return true;
}
