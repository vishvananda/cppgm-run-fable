#pragma once

#include <map>
#include <string>
#include <vector>

#include "sema/scope.h"
#include "sema/template_info.h"
#include "sema/type.h"

// Shared internals of the Itanium object-name mangler, split between
// lower_name.cpp (scope/type spellings) and lower_name_template.cpp
// (function-template signatures and written dependent forms). These
// are implementation details of the lowering name layer; public entry
// points stay in lower_name.h.

struct AstTypeId;
struct AstTemplateArgument;

namespace lower_mangle {

using std::string;
using std::vector;

std::runtime_error OutsideBoundary(const char* what);

// One component of a qualified name. A class-template specialization
// component carries the template name plus its argument list and
// mangles as `NameI<args>E` (5.1.8), with the template name and the
// template-id both substitution candidates. `pack_start`/`pack_end`
// bound the flattened argument run a template parameter pack owns
// (5.1.8: pack arguments spell `J <args> E`).
struct NameComponent
{
	NameComponent() : args(0), pack_start((size_t)-1), pack_end(0) {}

	string name;
	const vector<TemplateArg>* args;
	size_t pack_start;
	size_t pack_end;
};

// The flattened argument span the template's parameter pack owns;
// `start` stays (size_t)-1 when there is no pack (or the argument
// list is shorter than the non-pack parameters).
void ArgsPackSpan(const vector<TemplateParam>& params, size_t arg_count,
                  size_t& start, size_t& end);

// The named components (namespaces and classes) from the global scope
// down to `scope`, outermost first.
vector<NameComponent> ScopeComponents(const Scope* scope);

// 5.1.4.2: whether the component list opens with the global ::std
// namespace (spelled St, never a substitution candidate).
bool HeadIsStd(const vector<NameComponent>& parts);

// Component substitution table (5.1.9): previously seen substitutable
// fragments compress to S_/S<n>_.
class Substitutions
{
public:
	// Returns the compressed spelling if `key` was seen, else "".
	string Find(const string& key) const
	{
		for (size_t i = 0; i < seen_.size(); i++)
		{
			if (seen_[i] != key)
				continue;
			if (i == 0)
				return "S_";
			return "S" + Base36(i - 1) + "_";
		}
		return "";
	}

	void Add(const string& key)
	{
		if (Find(key).empty())
			seen_.push_back(key);
	}

private:
	static string Base36(size_t value)
	{
		const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		string out;
		do
		{
			out.insert(out.begin(), digits[value % 36]);
			value /= 36;
		} while (value);
		return out;
	}

	vector<string> seen_;
	std::map<string, int> param_instance_;

public:
	// The embedded-encoding instance counter (5.1.7 Z...E forms bump
	// it): a bare template-parameter substitution only compresses
	// against a spelling from the same encoding instance, while
	// composites over the parameter compress across instances (the
	// checked g++ encodings pin both behaviors).
	int encoding_instance = 0;

	string FindParam(const string& key)
	{
		std::map<string, int>::const_iterator found =
			param_instance_.find(key);
		if (found == param_instance_.end() ||
		    found->second != encoding_instance)
			return "";
		return Find(key);
	}

	void AddParam(const string& key)
	{
		Add(key);
		param_instance_[key] = encoding_instance;
	}

	// The enclosing function template's parameters while its abstract
	// pattern mangles: deferred dependent type-ids (14.5.2 written
	// forms kept as ASTs) spell parameter references T_/Tn_ through
	// this list.
	const vector<TemplateParam>* pattern_params = 0;
	// PA23: the written function-parameter names while the signature
	// mangles: decltype return expressions spell fp_/fp<n-1>_
	// references through this list.
	const vector<string>* fn_param_names = 0;
	// PA32 alias transparency (14.5.7p2: an alias template-id is
	// equivalent to the aliased type): written names resolve against
	// this scope chain so alias template-ids expand to their aliased
	// type-ids while mangling.
	const Scope* written_scope = 0;
	// The active alias-expansion frame: the alias's parameter names
	// bind the use site's written arguments, spelled in the enclosing
	// context captured here.
	struct AliasFrame
	{
		const TemplateInfo* alias = 0;
		const vector<AstTemplateArgument>* args = 0;
		// PA33: a typed pattern expansion binds the alias parameters
		// to resolved TemplateArgs instead of written arguments.
		const vector<TemplateArg>* typed_args = 0;
		const vector<TemplateParam>* outer_params = 0;
		const vector<string>* outer_fn_params = 0;
		const Scope* outer_scope = 0;
		const AliasFrame* outer = 0;
	};
	const AliasFrame* alias_frame = 0;
};

string SourceName(const string& name);

// The Itanium <operator-name> code of an operator's source spelling
// ("-" -> "mi"); false when the spelling has no binary/unary code.
bool LookupOperatorCode(const string& text, string& code);

// A pointer-identity substitution key fragment (stable within one
// mangled name, independent of substitution-dependent spellings).
string PointerKey(const void* p);

// The structural substitution keys of one name component (the leaf
// name alone and the full chain through `prev`).
void ComponentKeys(const NameComponent& part, const string& prev,
                   const vector<TemplateParam>* params, string& name_key,
                   string& full_key);

string MangleSubstitutable(const string& key, const string& spelling,
                           Substitutions& subs);

string MangleType(const TypePtr& type, Substitutions& subs,
                  string* key_out = 0);

// Appends the parameter encodings of a function type ("v" when empty,
// trailing "z" when variadic); abstract pack-expansion parameters
// spell `Dp<element>`.
string MangleBareParameters(const TypePtr& fn, Substitutions& subs,
                            string* key_out = 0);

// One template-argument list with the parameter pack's run wrapped in
// `J <args> E` (an empty pack still spells `JE`).
string MangleArgList(const vector<TemplateArg>& args, size_t pack_start,
                     size_t pack_end, Substitutions& subs);

// One template argument (5.1.6/5.1.7).
string MangleTemplateArg(const TemplateArg& arg, Substitutions& subs);

// Appends the enclosing components of a symbol's nested-name prefix,
// registering each as a substitution candidate. Returns the prefix
// key for the terminal component.
string ManglePrefixComponents(const vector<NameComponent>& parts,
                              Substitutions& subs, string& encoding);

string MangleTerminalName(const string& name, size_t arity);

// PA33 14.5.7p2 (lower_name_template.cpp): an alias-template
// specialization pattern is the aliased type - the alias's written
// target mangles with its parameters bound to the resolved pattern
// arguments.
string MangleTypedAliasExpansion(const TemplateInfo& alias,
                                 const vector<TemplateArg>& args,
                                 Substitutions& subs, string* key_out);

// 5.1.8 dependent names (lower_name_template.cpp): a written type-id
// (dependent qualified names, dependent template-ids, pointer /
// reference declarator suffixes, pack expansions) mangles
// syntactically with template-parameter references spelled T_/Tn_.
string MangleDependentTypeId(const AstTypeId& id, Substitutions& subs,
                             string* key_out);

}  // namespace lower_mangle
