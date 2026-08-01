#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sema/type.h"
#include "sema/type_builder.h"

// PA15 body-binding state (sem_class.cpp / sem_binder.cpp): the queued
// in-class member-body record and the open function context. SemBinder
// owns instances of these; they carry no behavior of their own.

struct Scope;
struct ClassInfo;
struct SemNode;
struct TemplateInfo;
struct AstTemplateParameter;

// One queued in-class member-function (or hidden-friend) body,
// analyzed when the outermost enclosing class completes (9.2p2).
struct SemDeferredBody
{
	const AstDecl* decl = 0;
	DeclaratorInfo composed;  // DK_FUNCTION methods / friends
	std::string name;
	Scope* fn_scope = 0;
	Scope* declaring = 0;  // class scope (methods) / namespace (friends)
	ClassInfo* cls = 0;    // lexical class context
	bool is_friend = false;
	bool is_static = false;
	bool out_of_class = false;  // qualified definition: strong emission
	// The qualified definition spelled `inline`: it still prints,
	// but weak (7.1.2p4 linkage with the reference's presentation).
	bool spelled_inline = false;
	// PA36: a member-template specialization body (constructor
	// templates); extern-template class suppression skips it.
	bool member_template = false;
};

// The current function context while a body is analyzed: the member
// class (methods), the lexical class (hidden friends), and the
// function's own identity for friendship checks.
struct SemMethodContext
{
	SemMethodContext() : cls(0), lexical_cls(0), fn_scope(0), fn_owner(0)
	{}

	const ClassInfo* cls;          // null outside member functions
	const ClassInfo* lexical_cls;  // hidden friend's lexical class
	TypePtr this_type;  // pointer to cv class (null outside methods)
	Scope* fn_scope;
	const Scope* fn_owner;  // declaring scope of the open function
	std::string fn_name;
	// PA21: the template name behind an instantiated specialization
	// body ("read" while binding read<int>); friend grants recorded
	// under the template name match through it.
	std::string fn_template_name;
};

// One clause expansion of a pack parameter: the declared pack name
// and its expanded slots.
struct SemPackParamRecord
{
	std::string name;
	std::vector<std::string> names;
	std::vector<TypePtr> types;
};

// A captureless closure's synthesized function identity.
struct SemClosureFunction
{
	const Scope* owner = 0;
	std::string name;
	TypePtr type;
};

// A poisoned instantiated body queued for an end-of-unit retry.
struct SemRetryBody
{
	SemRetryBody() : deferred_index(0) {}

	SemDeferredBody body;
	size_t deferred_index;
};

// A deferred member-class definition of an instantiated class
// (14.7.1p1), completed on demand.
struct SemPendingClassDefinition
{
	SemPendingClassDefinition() : decl(0), scope(0) {}

	const AstDecl* decl;
	Scope* scope;  // the class scope the definition binds in
};

// PA39/CWG 1330 (template_body.cpp): a definition node built inside a
// replay window (an in-class member body flushed by the replayed
// class's own CompleteClass) records its pending noexcept spec here;
// the facts resolve promote-only after the end-of-unit retry fixpoint,
// before the lowering reads them. Without this the definition keeps
// may-throw while its call sites resolve the binding's record to
// noexcept - the callee then lacks the 15.4p9 terminate barrier its
// callers assume.
struct SemPendingNodeFact
{
	SemPendingNodeFact() : node(0), expr(0), scope(0) {}

	SemNode* node;
	const AstExpr* expr;
	Scope* scope;
};

// PA34 builtin shadow templates (sem_builtin_template.cpp), built on
// first demand: the __type_pack_element record with the synthesized
// AST for its index parameter's declared type, and the
// __is_nothrow_invocable record.
struct SemBuiltinTemplates
{
	std::unique_ptr<TemplateInfo> type_pack_element;
	std::unique_ptr<AstTemplateParameter> type_pack_index_param;
	std::unique_ptr<TemplateInfo> nothrow_invocable;
};
