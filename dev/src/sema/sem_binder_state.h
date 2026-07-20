#pragma once

#include <string>

#include "sema/type.h"
#include "sema/type_builder.h"

// PA15 body-binding state (sem_class.cpp / sem_binder.cpp): the queued
// in-class member-body record and the open function context. SemBinder
// owns instances of these; they carry no behavior of their own.

struct Scope;
struct ClassInfo;

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
