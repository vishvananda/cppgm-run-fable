#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
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
	TMPL_FUNCTION,
	TMPL_VARIABLE,
	// PA21 alias templates: no specializations of their own (14.5.7);
	// a use substitutes the arguments into the aliased type-id.
	TMPL_ALIAS
};

// One parameter of a template head: a type parameter, a PA19 integral
// non-type (value) parameter, or a PA21 template-template parameter,
// optionally defaulted, and optionally a pack.
enum ETemplateParamKind
{
	TPK_TYPE,
	TPK_VALUE,
	TPK_TEMPLATE
};

struct TemplateParam
{
	TemplateParam()
		: kind(TPK_TYPE), pack(false), default_type(0), source(0),
		  default_expr(0)
	{}

	ETemplateParamKind kind;
	bool pack;
	string name;                   // empty for unnamed parameters
	const AstTypeId* default_type; // TPK_TYPE: null when no default
	// TPK_VALUE: the parameter's AST (the declared type composes on
	// demand - it may reference earlier parameters, `template<class T,
	// T v>`) and the default expression when present.
	const AstTemplateParameter* source;
	const AstExpr* default_expr;
	// TPK_TEMPLATE: the parameter's own template parameter list
	// (arity/kind matching per 14.3.3; names inside it are unused).
	vector<TemplateParam> tt_params;
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
	vector<TemplateArg> args;
	string key;
	NamedTypeInfo* entity;
	Scope* param_scope;  // argument alias scope (parent of class scope)
	ScopeBinding self;
	bool instantiated;   // pattern body bound (set before member binding
	                     // so current-instantiation uses resolve here)
	// PA19 explicit/partial specialization: the body bound for this
	// key came from an explicit specialization definition or a partial
	// specialization pattern (the primary's registered member
	// definitions do not apply then).
	bool explicit_spec = false;
	bool from_partial = false;
	// PA19 14.7.1: static-data-member definitions instantiate on
	// demand (odr-use of the member, or definition of an object of
	// this specialization); set once an object definition demanded
	// them all so later-registered definitions instantiate on sight.
	bool statics_demanded = false;
	// PA21 14.7.2: named by an `extern template` declaration (a later
	// explicit-instantiation definition emits on ordinary demand
	// without the emission-root marking).
	bool extern_declared = false;
	// PA21 14.7.3p6: the instantiation was used in a way that pins its
	// definition (member lookup, completeness); a later explicit
	// specialization is then ill-formed rather than refreshing.
	bool hard_used = false;
	// Indices into owner->member_defs already instantiated for this
	// specialization.
	map<size_t, bool> members_done;
	// PA21 14.7.3: member names owned by explicit specializations
	// (`template<> int tag<int>::id() ...`); the primary's registered
	// member definitions do not instantiate for them.
	map<string, bool> member_spec_names;
	// PA21: the partial specialization this body came from (index into
	// owner->partials; -1 for the primary) with its deduced parameter
	// slots, and the partial member definitions already instantiated.
	int partial_index = -1;
	vector<TemplateArg> partial_bound;
	map<size_t, bool> partial_members_done;
};

// One instantiated function-template specialization: the concrete
// signature under its specialization name ("f<int>"). `self` is the
// single-overload SB_FUNCTION binding used to route the call through
// the ordinary callee machinery.
struct FunctionSpecialization
{
	FunctionSpecialization()
		: owner(0), param_scope(0), odr_used(false), body_emitted(false)
	{}

	TemplateInfo* owner;
	vector<TemplateArg> args;
	string key;
	string name;   // entity name: template-name + argument spellings
	TypePtr type;  // concrete namespace-scope function type
	vector<TypePtr> declared_params;  // pre-adjustment parameter types
	Scope* param_scope;  // argument alias scope (body + defaults bind here)
	ScopeBinding self;
	// 14.7.1p2: the body instantiates on the first odr-use (deduction
	// alone composes only the signature); an odr-used specialization
	// still undefined at the end of the unit is an error.
	bool odr_used;
	bool body_emitted;
	// PA19 14.7.3: the explicit-specialization definition owning this
	// key (its body replaces the primary pattern's; non-inline
	// explicit definitions emit strong).
	const AstDecl* explicit_def = 0;
	bool explicit_inline = false;
	// PA21 14.7.2: named by an `extern template` declaration (the
	// definition lives elsewhere; local uses reference it) and/or an
	// explicit-instantiation definition (emitted as an object root).
	bool extern_suppressed = false;
	bool inst_definition = false;
};

// PA19: one partial specialization of a class or variable template:
// its own parameter list, the argument pattern (over abstract
// placeholder slots), and the defining declaration.
struct PartialSpecialization
{
	PartialSpecialization() : decl(0), init(0) {}

	vector<TemplateParam> params;
	vector<TemplateArg> pattern;
	// Canonical key of `pattern`, computed once at registration so
	// redeclaration merging and member-definition matching never
	// rebuild it per candidate.
	string pattern_key;
	const AstDecl* decl;      // class definition (TMPL_CLASS)
	const AstExpr* init;      // initializer (TMPL_VARIABLE)
	// PA21: out-of-class member definitions of this partial
	// specialization (qualified by its template-id), in source order.
	vector<const AstDecl*> member_defs;
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
	// PA19: which function parameters are pack-expanded (aligned with
	// param_patterns; the pattern entry is the element pattern).
	vector<bool> param_pattern_packs;
	// PA21: the first declaration's parameter names (slot-name
	// fallback for unnamed definition parameters).
	vector<string> declared_param_names;

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
	// PA19 partial specializations in declaration order.
	vector<PartialSpecialization> partials;
	// PA21 alias templates: the aliased type-id AST (substitution
	// target).
	const AstTypeId* alias_type = 0;
	// PA21 template-template placeholder: the enclosing template's
	// parameter slot this pattern-scope template stands for (-1 for
	// real templates).
	int tt_param_index = -1;
	// PA21 member templates: the class-template member scope this
	// template was captured in during an enclosing instantiation, and
	// the in-class pattern declaration that captured it (registration
	// of an out-of-class definition re-finds the record through it).
	const AstDecl* member_pattern = 0;
	// PA21 member function templates: the owning class entity (null
	// for namespace-scope templates), the declared staticness, and the
	// declared member access.
	const NamedTypeInfo* member_of = 0;
	bool member_static = false;
	EMemberAccess member_access = MA_PUBLIC;
	// PA19 variable templates: the primary initializer and declared
	// type AST, explicit-specialization initializers by key, and the
	// resolved constant binding per argument key.
	const AstDecl* var_decl = 0;
	const AstExpr* var_init = 0;
	map<string, const AstExpr*> var_explicit;
	map<string, unique_ptr<ScopeBinding>> var_specs;
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

// Cyclic or runaway instantiation guard, far above any supported
// nesting.
enum { kTemplateInstantiationDepthLimit = 200 };

// PA21 14.3.3: whether a template with parameter list `actual`
// matches a template-template parameter declared with `formal`
// (positional kind/pack matching; a formal pack absorbs the rest).
bool TemplateTemplateParamsMatch(const vector<TemplateParam>& formal,
                                 const vector<TemplateParam>& actual);

// PA19 packs: the index of the parameter pack (params.size() when
// none) and the mapping of a resolved flattened argument list onto
// the parameter list (the pack owns the middle run).
size_t TemplatePackIndex(const vector<TemplateParam>& params);
bool MapParamSpans(const vector<TemplateParam>& params, size_t argc,
                   vector<std::pair<size_t, size_t>>& spans);

// A stable canonical key for one template-argument list (never
// printed; entity pointers keep it unique within the translation
// unit).
string TemplateArgumentKey(const vector<TemplateArg>& args);

// The flattened `text` with each template-parameter name replaced by
// its positional marker (14.1: parameter identity is positional).
// Whole-identifier replacement; 14.6.1p6 guarantees no other entity
// in a template body carries a parameter's name.
string PositionalizeTemplateNames(const string& text,
                                  const vector<TemplateParam>& params);

// The canonical parameter-clause spelling of a function declarator:
// per-parameter declared-type text with the declarator-ids stripped
// and the template-parameter names positionalized, so declarations
// with renamed parameters compare structurally.
string CanonicalDeclaratorParams(const AstDeclarator& declarator,
                                 const vector<TemplateParam>& params);

// The source-like spelling of one template-argument list, used for
// specialization entity names ("Box<int>"): the lowering's sanitized
// symbol names and scope paths derive from it.
string TemplateArgumentSpelling(const vector<TemplateArg>& args);

// Whether two template parameter lists match positionally in kind
// and pack-ness (template-template parameters recursively).
bool SameTemplateParameterKinds(const vector<TemplateParam>& a,
                                const vector<TemplateParam>& b);

// 14.5.6.1p6: whether two template parameter lists declare the same
// template head - positionally matching kinds and pack-ness, with
// non-type parameter types compared by positionalized spelling
// (enable_if-style defaulted parameters with different condition
// spellings declare distinct overloads). Defaults are not identity.
bool SameTemplateParameterLists(const vector<TemplateParam>& a,
                                const vector<TemplateParam>& b);
