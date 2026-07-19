#pragma once

#include "sema/sem_binder.h"

// Saved-and-cleared binder state around one instantiation: the
// instantiated declarations bind in their own context (the template's
// lexical scope), never into the open class/function/dump state of the
// use site. `instantiating` marks a body/definition instantiation
// (weak emission); signature composition passes false and inherits
// the surrounding mode. The destructor restores everything
// (exception-safe); bodies live in sem_template.cpp.
struct SemBinder::InstantiationContext
{
	InstantiationContext(SemBinder& binder, Scope* scope,
	                     bool instantiating = false);
	~InstantiationContext();

private:
	SemBinder& binder_;
	Scope* saved_scope_;
	std::vector<TypePtr>* saved_fields_;
	EMemberAccess saved_access_;
	bool saved_c_linkage_;
	MethodContext saved_method_;
	TypePtr saved_return_;
	TypePtr saved_return_pattern_;
	int saved_hidden_names_;
	bool saved_bit_field_;
	bool saved_instantiating_;
	bool saved_unevaluated_;
	Scope* saved_param_capture_;
	std::map<unsigned long long, bool> saved_bf_units_;
	vector<SemNode*> saved_parents_;
	vector<ClassInfo*> saved_open_classes_;
	vector<DeferredBody> saved_deferred_;
};

