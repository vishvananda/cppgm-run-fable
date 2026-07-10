#pragma once

#include <string>

using std::string;

#include "sema/scope.h"
#include "sema/template_info.h"
#include "sema/type.h"

// PA14 symbol naming: deterministic LowIR symbol spellings derived
// from the resolved scope model, plus the Itanium object-name encoding
// carried as `object=` metadata for later object emission.

// The enclosing named-namespace path of `scope` joined with "__"
// (trailing separator included; unnamed components are skipped).
string LowerScopePath(const Scope* scope);

// The program-wide identity key of the namespace path of `scope`:
// components joined with "::" (which no identifier can contain, so
// nested paths cannot collide with longer names), with each unnamed
// namespace keyed by its per-translation-unit scope object so internal
// entities of different units stay distinct.
string LowerScopeKey(const Scope* scope);

// A declared name reduced to LowIR identifier characters
// ("operator delete" -> "operatordelete").
string LowerSanitizeName(const string& name);

// The position of the overload with type `type` in the declaration
// -ordered overload set of (scope, name); 0 for the first overload and
// for non-overloaded names.
size_t LowerOverloadIndex(const Scope* scope, const string& name,
                          const TypePtr& type);

// True when that overload was declared `= delete`.
bool LowerOverloadDeleted(const Scope* scope, const string& name,
                          const TypePtr& type);

// True when `scope` is inside an unnamed namespace (internal linkage).
bool LowerInUnnamedNamespace(const Scope* scope);

// Itanium-mangled object names. Variables use the course spelling
// (_Z<name> at the global scope, _ZN...E inside namespaces).
string MangleFunctionObjectName(const Scope* scope, const string& name,
                                const TypePtr& type);

// PA18: the Itanium spelling of a function-template specialization
// (`_Z1fIiEvT_`): template-argument list and pattern-based signature
// with the return type included.
string MangleFunctionTemplateObjectName(const FunctionSpecialization& spec);
string MangleVariableObjectName(const Scope* scope, const string& name);

// PA23: the Itanium spelling of a variable-template specialization
// (`_ZN4propILi0EE14static_query_vI2exS0_EE`): the terminal is a
// template-id with the template name a substitution candidate.
string MangleVariableTemplateObjectName(const Scope* scope,
                                        const TemplateInfo& tmpl,
                                        const vector<TemplateArg>& args);

// PA15: the Itanium spelling of a non-static member function,
// constructor, or destructor. `type` is the this-adjusted function
// type (the implicit object pointer first); `special_code` is
// "C1"/"C2"/"D1"/"D2" for constructor/destructor entries, "" for
// ordinary methods.
// The declaration-position overload index of a member function whose
// entry type is this-adjusted (0 for the primary declaration).
size_t LowerMemberOverloadIndex(const Scope* scope, const string& name,
                                const TypePtr& adjusted);

string MangleMemberFunctionObjectName(const Scope* scope,
                                      const string& name,
                                      const TypePtr& type,
                                      const string& special_code);

// PA21: the Itanium spelling of a constructor-template specialization
// entry (`C1I<args>E` with pattern-based parameters).
string MangleMemberFunctionTemplateObjectName(
	const Scope* scope, const FunctionSpecialization& spec,
	const string& special_code);

// PA17: the bare Itanium encoding of a class entity ("2YD",
// "N2ns1XE"); vtable/typeinfo object names prefix it with _ZTV, _ZTI,
// or _ZTS, and the typeinfo-name data holds its characters.
string MangleClassTypeEncoding(const NamedTypeInfo* entity);

// PA25: the bare Itanium encoding of an arbitrary type ("i", "Pi",
// "A4_c"); typeid RTTI records for non-class operands key on it.
string MangleTypeEncoding(const TypePtr& type);
