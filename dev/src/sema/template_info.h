#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

using std::map;
using std::string;
using std::unique_ptr;
using std::vector;

#include "ast/ast.h"
#include "sema/scope.h"
#include "sema/type.h"

// PA18 template model: the captured template declarations of one
// translation unit and their instantiated specializations. A template
// body is never analyzed at declaration; the binder re-walks the
// stored AST at each instantiation with the parameter names aliased to
// the concrete argument types, so the ordinary PA11-PA17 declaration,
// class, and expression machinery does all the semantic work. The AST
// outlives binding and lowering (the driver keeps it on the task), so
// the records hold plain AST pointers.

enum ETemplateKind
{
	TMPL_CLASS,
	TMPL_FUNCTION
};

// One type-parameter of a template head (the PA18 subset: type
// parameters only, optionally defaulted).
struct TemplateParam
{
	TemplateParam() : default_type(0) {}

	string name;                   // empty for unnamed parameters
	const AstTypeId* default_type; // null when no default
};

struct TemplateInfo;

// One instantiated class-template specialization. `self` is the
// SB_TYPE binding name resolution returns for the template-id; it
// lives here (not in any scope) so scope dumps and unqualified lookup
// never see it.
struct ClassSpecialization
{
	ClassSpecialization()
		: owner(0), entity(0), param_scope(0), instantiated(false)
	{}

	TemplateInfo* owner;
	vector<TypePtr> args;
	string key;
	NamedTypeInfo* entity;
	Scope* param_scope;  // argument alias scope (parent of class scope)
	ScopeBinding self;
	bool instantiated;   // pattern body bound (set before member binding
	                     // so current-instantiation uses resolve here)
	// Indices into owner->member_defs already instantiated for this
	// specialization.
	map<size_t, bool> members_done;
};

// One instantiated function-template specialization: the concrete
// signature under its specialization name ("f<int>"). `self` is the
// single-overload SB_FUNCTION binding used to route the call through
// the ordinary callee machinery.
struct FunctionSpecialization
{
	FunctionSpecialization()
		: owner(0), param_scope(0), body_emitted(false)
	{}

	TemplateInfo* owner;
	vector<TypePtr> args;
	string key;
	string name;   // entity name: template-name + argument spellings
	TypePtr type;  // concrete namespace-scope function type
	vector<TypePtr> declared_params;  // pre-adjustment parameter types
	Scope* param_scope;  // argument alias scope (body + defaults bind here)
	ScopeBinding self;
	bool body_emitted;
};

// One captured template declaration (class or function). Forward
// declarations and the definition of the same class template merge
// onto one record; the definition's AST becomes the pattern
// (parameter identity is positional, 14.1).
struct TemplateInfo
{
	TemplateInfo()
		: kind(TMPL_CLASS), declaring(0), decl(0), pattern_decl(0),
		  has_definition(false), anchor(0), pattern_ready(false)
	{}

	ETemplateKind kind;
	string name;
	Scope* declaring;      // lexical scope of the declaration
	const AstDecl* decl;   // the DK_TEMPLATE node (definition once seen)
	// The declaration inside `decl` (DK_CLASS / DK_FUNCTION / DK_SIMPLE).
	const AstDecl* pattern_decl;
	bool has_definition;
	vector<TemplateParam> params;
	// The template's identity entity: TK_TEMPLATE_SPEC pattern types
	// name it, and instantiated entities point back through
	// NamedTypeInfo::spec_template.
	NamedTypeInfo* anchor;

	// --- function templates ---
	// Abstract signature composed with positional placeholder types
	// (deduction patterns and mangling). `pattern` stays null when the
	// full signature cannot compose abstractly; `param_patterns` holds
	// the per-parameter declared-type patterns (null entries are
	// non-deduced contexts, substituted after deduction).
	bool pattern_ready;
	TypePtr pattern;
	vector<TypePtr> param_patterns;

	// --- class templates ---
	// Out-of-class member definitions seen so far (DK_TEMPLATE nodes
	// whose declarator is qualified by this template's template-id), in
	// source order.
	vector<const AstDecl*> member_defs;

	// Unqualified names the definition-time sanity walk could not
	// account for; re-checked at the end of the translation unit.
	vector<string> suspicious_names;

	map<string, unique_ptr<ClassSpecialization>> class_specs;
	map<string, unique_ptr<FunctionSpecialization>> fn_specs;
	// Dependent uses of this class template (`box<T>` with abstract
	// arguments): one stable SB_TYPE binding per argument pattern,
	// carrying a TK_TEMPLATE_SPEC type.
	map<string, unique_ptr<ScopeBinding>> dependent_uses;
};

// The per-translation-unit owner of every TemplateInfo.
class TemplateRegistry
{
public:
	const vector<unique_ptr<TemplateInfo>>& All() const
	{
		return all_;
	}

	TemplateInfo* Create(ETemplateKind kind, const string& name,
	                     Scope* declaring)
	{
		all_.push_back(unique_ptr<TemplateInfo>(new TemplateInfo()));
		TemplateInfo* info = all_.back().get();
		info->kind = kind;
		info->name = name;
		info->declaring = declaring;
		return info;
	}

private:
	vector<unique_ptr<TemplateInfo>> all_;
};

// A stable canonical key for one template-argument list (never
// printed; entity pointers keep it unique within the translation
// unit).
string TemplateArgumentKey(const vector<TypePtr>& args);

// The source-like spelling of one template-argument list, used for
// specialization entity names ("Box<int>"): the lowering's sanitized
// symbol names and scope paths derive from it.
string TemplateArgumentSpelling(const vector<TypePtr>& args);
