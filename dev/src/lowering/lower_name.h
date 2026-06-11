#pragma once

#include <string>

using std::string;

#include "sema/scope.h"
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
string MangleVariableObjectName(const Scope* scope, const string& name);
