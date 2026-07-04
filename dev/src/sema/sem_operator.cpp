#include "sema/sem_expr.h"

#include <set>
#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;
using std::set;

// PA15 ordinary (non-template) operator overloading: candidate
// collection over the member operators of the left operand plus the
// namespace-scope operators found by ordinary lookup and
// argument-dependent lookup (hidden friends included), 13.3.1.2
// selection, and the resolved call node.

namespace {

// 13.6: whether a built-in candidate competes with the user-declared
// operators for this spelling (the supported arithmetic/comparison
// binary forms).
bool BuiltinCandidateOp(const string& spelling)
{
	return spelling == "==" || spelling == "!=" || spelling == "<" ||
		spelling == ">" || spelling == "<=" || spelling == ">=" ||
		spelling == "+" || spelling == "-" || spelling == "*" ||
		spelling == "/" || spelling == "%" || spelling == "&" ||
		spelling == "|" || spelling == "^";
}

// The built-in-usable type of an operand: a class goes through its
// unique non-explicit conversion function to an arithmetic result
// (null when none or ambiguous); other operands pass through.
TypePtr BuiltinOperandType(const SemValue& value)
{
	TypePtr bare = RemoveTopCv(value.type);
	if (bare->kind != TK_CLASS)
		return IsArithmeticType(bare) || bare->kind == TK_ENUM
			? bare : TypePtr();
	if (!bare->named->class_record)
		return TypePtr();
	bool source_const = false;
	bool source_volatile = false;
	TopCv(value.type, source_const, source_volatile);
	const ClassConversion* found = 0;
	int found_score = 3;
	for (const ClassInfo* link = bare->named->class_record; link;
	     link = link->base)
		for (size_t i = 0; i < link->conversions.size(); i++)
		{
			const ClassConversion& conv = link->conversions[i];
			if (conv.is_explicit)
				continue;
			if (source_const && !conv.type->is_const)
				continue;
			int score = conv.type->is_const == source_const ? 0 : 1;
			if (found && score == found_score &&
			    !TypeEquals(found->result, conv.result))
				return TypePtr();
			if (!found || score < found_score)
			{
				found = &conv;
				found_score = score;
			}
		}
	if (!found)
		return TypePtr();
	TypePtr result = IsReferenceType(found->result)
		? found->result->target : found->result;
	result = RemoveTopCv(result);
	return IsArithmeticType(result) || result->kind == TK_ENUM
		? result : TypePtr();
}

// The innermost enclosing namespace of a declared entity (3.4.2p2:
// associated namespaces do not climb to parent namespaces).
const Scope* InnermostNamespace(const Scope* scope)
{
	while (scope && scope->kind != SCOPE_NAMESPACE)
		scope = scope->parent;
	return scope;
}

// 3.4.2p2 associated namespaces of one argument type (the PA15
// subset: classes with their base chains, enumerations, and the
// pointee chain).
void CollectAssociatedNamespaces(TypesModel& model, const TypePtr& type,
                                 vector<const Scope*>& out)
{
	if (!type)
		return;  // a template-only overload set has no operand type
	switch (type->kind)
	{
	case TK_CLASS:
		for (const NamedTypeInfo* entity = type->named; entity;
		     entity = entity->base_entity)
		{
			const Scope* ns = InnermostNamespace(entity->scope);
			if (ns)
				out.push_back(ns);
			// 3.4.2p2: a class-template specialization associates the
			// namespaces of its type template arguments and the
			// namespaces of its template template arguments (value
			// arguments contribute nothing).
			for (size_t i = 0; i < entity->spec_args.size(); i++)
			{
				const TemplateArg& arg = entity->spec_args[i];
				if (arg.template_entity)
				{
					const Scope* tns =
						InnermostNamespace(arg.template_entity->declaring);
					if (tns)
						out.push_back(tns);
				}
				else if (!arg.is_value)
					CollectAssociatedNamespaces(model, arg.type, out);
			}
		}
		return;
	case TK_ENUM:
	{
		const Scope* ns = InnermostNamespace(type->named->scope);
		if (ns)
			out.push_back(ns);
		return;
	}
	case TK_POINTER:
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	case TK_ARRAY:
		CollectAssociatedNamespaces(model, type->target, out);
		return;
	default:
		return;
	}
}

void AppendBindingOverloads(const ScopeBinding& binding, bool is_member,
                            bool include_hidden,
                            vector<OperatorCandidate>& out,
                            set<const void*>& seen)
{
	// PA18: a name declared only by function templates has no ordinary
	// overload entries.
	size_t count = binding.type ? binding.overloads.size() + 1 : 0;
	for (size_t i = 0; i < count; i++)
	{
		if (!include_hidden && i < binding.fn_adl_only.size() &&
		    binding.fn_adl_only[i])
			continue;
		const TypePtr& declared =
			i == 0 ? binding.type : binding.overloads[i - 1];
		const void* key = declared.get();
		if (!seen.insert(key).second)
			continue;
		OperatorCandidate candidate;
		candidate.binding = &binding;
		candidate.index = i;
		candidate.is_member = is_member;
		candidate.declared = declared;
		out.push_back(candidate);
	}
}

}  // namespace

// PA18: the scope a binding's default arguments analyze in (see
// sem_expr.h).
Scope* TemplateDefaultArgScope(const ScopeBinding& binding)
{
	for (const Scope* scope = binding.home; scope; scope = scope->parent)
		if (scope->kind == SCOPE_TEMPLATE_PARAMS)
			return const_cast<Scope*>(binding.home);
	return 0;
}

// PA18: deduce every function template of `binding` against the
// argument list; successful deductions join as ordinary candidates
// carrying their specialization identity.
void SemExprAnalyzer::AppendTemplateCandidates(
	const ScopeBinding& binding, const vector<SemValue>& args,
	vector<OperatorCandidate>& out, set<const void*>& seen,
	const AstNamePart* explicit_part, const Scope* declared_in)
{
	for (size_t t = 0; t < binding.fn_templates.size(); t++)
	{
		if (declared_in)
		{
			// A friend's declaring may chain through an enclosing
			// instantiation's alias scope to its home namespace.
			const Scope* home = binding.fn_templates[t]->declaring;
			while (home && home->kind == SCOPE_TEMPLATE_PARAMS)
				home = home->parent;
			if (home != declared_in)
				continue;
		}
		const FunctionSpecialization* spec =
			host_.DeduceFunctionTemplate(*binding.fn_templates[t], args,
			                             explicit_part);
		if (!spec)
			continue;
		if (!seen.insert(&spec->self).second)
			continue;
		OperatorCandidate candidate;
		candidate.binding = &spec->self;
		candidate.index = 0;
		candidate.is_member = false;
		candidate.declared = spec->type;
		candidate.spec = spec;
		out.push_back(candidate);
	}
}

// PA21 member templates: deduce a class scope's member operator (or
// member function) templates against the explicit operands (the
// object binds as the implicit parameter during ranking).
void SemExprAnalyzer::AppendMemberTemplateCandidates(
	const ScopeBinding& binding, const vector<SemValue>& operands,
	vector<OperatorCandidate>& out, set<const void*>& seen)
{
	if (binding.fn_templates.empty() || operands.empty())
		return;
	// 12.8p19-adjacent (the checked shape): assigning a same-class
	// value uses the copy/move assignment special member; an
	// assignment operator template never declares one and does not
	// compete for it.
	if (binding.name == "operator =" && operands.size() == 2 &&
	    operands[0].type && operands[1].type)
	{
		TypePtr rhs = RemoveTopCv(operands[1].type);
		if (IsReferenceType(rhs))
			rhs = RemoveTopCv(rhs->target);
		if (rhs->kind == TK_CLASS &&
		    rhs->named == RemoveTopCv(operands[0].type)->named)
			return;
	}
	vector<SemValue> shells;
	for (size_t i = 1; i < operands.size(); i++)
	{
		SemValue shell;
		shell.type = operands[i].type;
		shell.category = operands[i].category;
		shells.push_back(std::move(shell));
	}
	for (size_t t = 0; t < binding.fn_templates.size(); t++)
	{
		const FunctionSpecialization* spec =
			host_.DeduceFunctionTemplate(*binding.fn_templates[t],
			                             shells, 0);
		if (!spec)
			continue;
		if (!seen.insert(&spec->self).second)
			continue;
		OperatorCandidate candidate;
		candidate.binding = &spec->self;
		candidate.index = 0;
		candidate.is_member = true;
		candidate.declared = spec->type;
		candidate.spec = spec;
		out.push_back(candidate);
	}
}

// PA18 13.4p2: a function set used against a function-typed target
// gains the specializations its templates deduce from that target.
void SemExprAnalyzer::AddTargetDeducedOverloads(SemValue& value,
                                                const TypePtr& dest)
{
	if (!value.function_set || value.fn_templates.empty())
		return;
	TypePtr target = RemoveTopCv(dest);
	if (target->kind == TK_POINTER ||
	    target->kind == TK_LVALUE_REFERENCE ||
	    target->kind == TK_RVALUE_REFERENCE)
		target = target->target;
	if (target->kind != TK_FUNCTION)
		return;
	value.overload_specs.resize(value.overloads.size(), 0);
	for (size_t t = 0; t < value.fn_templates.size(); t++)
	{
		const FunctionSpecialization* spec =
			host_.DeduceFunctionTemplateFromTarget(
				*value.fn_templates[t], target);
		if (!spec)
			continue;
		// The set fills once per ranked candidate: a specialization
		// deduced for an earlier candidate must not join again (a
		// duplicate defeats the exactly-one-match selection rule).
		bool present = false;
		for (size_t i = 0; i < value.overload_specs.size(); i++)
			if (value.overload_specs[i] == spec)
				present = true;
		if (present)
			continue;
		value.overloads.push_back(spec->type);
		value.overload_specs.push_back(spec);
	}
}

void SemExprAnalyzer::DeduceFunctionSetArguments(
	vector<SemValue>& args, const vector<TypePtr>& candidates,
	vector<ConversionSource>& sources)
{
	bool augmented = false;
	for (size_t c = 0; c < candidates.size(); c++)
	{
		const TypePtr& fn = candidates[c];
		for (size_t i = 0;
		     i < args.size() && i < fn->parameters.size(); i++)
			if (args[i].function_set && !args[i].fn_templates.empty())
			{
				AddTargetDeducedOverloads(args[i], fn->parameters[i]);
				augmented = true;
			}
	}
	if (augmented)
	{
		sources.clear();
		for (size_t i = 0; i < args.size(); i++)
			sources.push_back(MakeConversionSource(args[i]));
	}
}

// Collects the user-declared candidates of `operator <spelling>` for
// the analyzed operand list.
void SemExprAnalyzer::CollectOperatorCandidates(
	const string& op_name, const vector<SemValue>& operands,
	bool member_only, vector<OperatorCandidate>& out)
{
	set<const void*> seen;
	if (!operands.empty() && operands[0].type->kind == TK_CLASS)
	{
		host_.RequireCompleteType(operands[0].type->named);
		Scope* members =
			host_.Model().MemberScope(operands[0].type->named);
		for (const Scope* link = members; link;
		     link = link->class_base)
			if (const ScopeBinding* own = FindOwnBinding(*link, op_name))
			{
				AppendBindingOverloads(*own, true, true, out, seen);
				// PA21: member operator templates deduce against the
				// explicit operands.
				AppendMemberTemplateCandidates(*own, operands, out,
				                               seen);
				break;  // derived declarations hide base ones (10.2)
			}
	}
	if (member_only)
		return;
	// Ordinary unqualified lookup (visible namespace operators).
	const ScopeBinding* visible =
		UnqualifiedLookup(host_.CurrentScope(), op_name, SLF_ANY);
	if (visible && visible->kind == SB_FUNCTION &&
	    visible->home && visible->home->kind != SCOPE_CLASS)
	{
		AppendBindingOverloads(*visible, false, false, out, seen);
		AppendTemplateCandidates(*visible, operands, out, seen);
	}
	// Argument-dependent lookup, hidden friends included.
	vector<const Scope*> namespaces;
	for (size_t i = 0; i < operands.size(); i++)
		CollectAssociatedNamespaces(host_.Model(), operands[i].type,
		                            namespaces);
	set<const Scope*> visited;
	for (size_t i = 0; i < namespaces.size(); i++)
	{
		if (!visited.insert(namespaces[i]).second)
			continue;
		const ScopeBinding* found =
			FindOwnBinding(*namespaces[i], op_name);
		if (found && found->kind == SB_FUNCTION)
		{
			AppendBindingOverloads(*found, false, true, out, seen);
			AppendTemplateCandidates(*found, operands, out, seen);
		}
	}
}

// 3.4.2: an unqualified call merges the visible overloads with the
// candidates argument-dependent lookup finds in the arguments'
// associated namespaces (hidden friends included; using-declaration
// imports into an associated namespace are not its own declarations).
SemValue SemExprAnalyzer::AnalyzeAdlCall(
	const AstExpr& expr, const string& name,
	const vector<const ScopeBinding*>& visible)
{
	vector<SemValue> args;
	vector<ConversionSource> sources;
	AnalyzeArgumentList(expr.arguments, args);
	for (size_t i = 0; i < args.size(); i++)
		sources.push_back(MakeConversionSource(args[i]));
	set<const void*> seen;
	vector<OperatorCandidate> candidates;
	for (size_t i = 0; i < visible.size(); i++)
	{
		AppendBindingOverloads(*visible[i], false, false, candidates,
		                       seen);
		AppendTemplateCandidates(*visible[i], args, candidates, seen);
	}
	vector<const Scope*> namespaces;
	for (size_t i = 0; i < args.size(); i++)
	{
		// 3.4.2: a class argument's associated entities include its
		// friends, so a dormant specialization instantiates first.
		TypePtr bare = args[i].type;
		if (bare && IsReferenceType(bare))
			bare = bare->target;
		if (bare)
			bare = RemoveTopCv(bare);
		if (bare && bare->kind == TK_CLASS)
			host_.RequireCompleteType(bare->named);
		CollectAssociatedNamespaces(host_.Model(), args[i].type,
		                            namespaces);
	}
	set<const Scope*> visited;
	for (size_t i = 0; i < namespaces.size(); i++)
	{
		if (!visited.insert(namespaces[i]).second)
			continue;
		const ScopeBinding* found = FindOwnBinding(*namespaces[i], name);
		if (found && found->kind == SB_FUNCTION)
		{
			if (found->owner == namespaces[i])
			{
				AppendBindingOverloads(*found, false, true, candidates,
				                       seen);
				AppendTemplateCandidates(*found, args, candidates, seen);
			}
			else
				// Friend templates declared here may ride a binding a
				// using-declaration owns (3.4.2p3 ignores the import,
				// not the friends).
				AppendTemplateCandidates(*found, args, candidates, seen,
				                         0, namespaces[i]);
		}
	}
	if (candidates.empty())
		throw runtime_error("undeclared name " + name);
	vector<TypePtr> declared;
	for (size_t c = 0; c < candidates.size(); c++)
		declared.push_back(candidates[c].declared);
	DeduceFunctionSetArguments(args, declared, sources);
	vector<TypePtr> ranking;
	vector<size_t> min_arity;
	for (size_t c = 0; c < candidates.size(); c++)
	{
		ranking.push_back(candidates[c].declared);
		size_t required = candidates[c].declared->parameters.size();
		const ScopeBinding& binding = *candidates[c].binding;
		const vector<const AstExpr*>* defaults =
			candidates[c].index < binding.fn_defaults.size()
				? &binding.fn_defaults[candidates[c].index] : 0;
		while (defaults && required > 0 && required <= defaults->size() &&
		       (*defaults)[required - 1])
			required--;
		min_arity.push_back(required);
	}
	vector<bool> is_template;
	vector<const FunctionSpecialization*> specs;
	for (size_t c = 0; c < candidates.size(); c++)
	{
		is_template.push_back(candidates[c].spec != 0);
		specs.push_back(candidates[c].spec);
	}
	vector<ImplicitConversion> conversions;
	SpecOverloadOrder order(host_, specs, sources.size());
	size_t winner = SelectBestOverload(ranking, sources, conversions,
	                                   &min_arity, &is_template, &order);
	const OperatorCandidate& chosen = candidates[winner];
	const ScopeBinding& binding = *chosen.binding;
	if (chosen.index < binding.fn_deleted.size() &&
	    binding.fn_deleted[chosen.index])
		throw runtime_error("use of deleted function " + name);
	const TypePtr& fn = chosen.declared;
	for (size_t i = 0; i < args.size(); i++)
		if (i < fn->parameters.size())
			ApplyConversion(args[i], conversions[i], fn->parameters[i]);
	for (size_t i = args.size(); i < fn->parameters.size(); i++)
	{
		Scope* default_scope = TemplateDefaultArgScope(binding);
		Scope* saved = default_scope
			? host_.SwapLookupScope(default_scope) : 0;
		SemValue filled;
		try
		{
			filled = Analyze(*binding.fn_defaults[chosen.index][i]);
		}
		catch (...)
		{
			if (default_scope)
				host_.SwapLookupScope(saved);
			throw;
		}
		if (default_scope)
			host_.SwapLookupScope(saved);
		CopyInitialize(filled, fn->parameters[i], "default argument");
		args.push_back(std::move(filled));
	}
	SemValue value = CallResult(fn);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding.owner, binding.name);
	callee->type = fn;
	callee->entity_scope = binding.owner;
	callee->entity_name = binding.name;
	callee->fn_spec = chosen.spec;
	host_.OnSpecializationOdrUsed(chosen.spec);
	if (chosen.index < binding.fn_unwind_no.size() &&
	    binding.fn_unwind_no[chosen.index])
		callee->unwind_no = true;
	if (chosen.index < binding.fn_noexcept_decl.size() &&
	    binding.fn_noexcept_decl[chosen.index])
		callee->noexcept_decl = true;
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

// 13.3.1.2 selection over the collected candidates; false when no
// user-declared candidate is viable (the caller falls back to the
// built-in meaning).
// The ranked signature of one user candidate: member operators fold
// the implicit object parameter in front of the declared parameters
// (13.3.1p2-p4 with 8.3.5p6 ref-qualifiers).
TypePtr SemExprAnalyzer::CandidateSignature(
	const OperatorCandidate& candidate, const SemValue& object)
{
	if (!candidate.is_member)
		return candidate.declared;
	const NamedTypeInfo* owner_entity =
		host_.Model().ScopeEntity(candidate.binding->owner);
	TypePtr class_type = MakeNamedType(
		TK_CLASS, owner_entity ? owner_entity : object.type->named);
	class_type = MakeCvQualifiedType(class_type,
	                                 candidate.declared->is_const,
	                                 candidate.declared->is_volatile);
	bool rvalue_param = candidate.declared->ref_qual == 2 ||
		(candidate.declared->ref_qual == 0 &&
		 object.category != VC_LVALUE);
	vector<TypePtr> parameters;
	parameters.push_back(
		MakeReferenceType(class_type, rvalue_param, true));
	for (size_t i = 0; i < candidate.declared->parameters.size(); i++)
		parameters.push_back(candidate.declared->parameters[i]);
	return MakeFunctionType(candidate.declared->target, parameters,
	                        candidate.declared->variadic);
}

// 13.3.1.2p3: the built-in operator form competes as a candidate when
// every operand has a built-in-usable type. Returns its ranking
// position ((size_t)-1 when absent).
size_t SemExprAnalyzer::AppendBuiltinCandidate(
	const string& spelling, const vector<SemValue>& operands,
	bool member_only, vector<TypePtr>& ranking,
	vector<size_t>& viable_arity)
{
	if (member_only || operands.size() != 2 ||
	    !BuiltinCandidateOp(spelling))
		return (size_t)-1;
	TypePtr left = BuiltinOperandType(operands[0]);
	TypePtr right = BuiltinOperandType(operands[1]);
	if (!left || !right)
		return (size_t)-1;
	TypePtr common = UsualArithmeticConversions(
		left->kind == TK_ENUM
			? MakeFundamentalType(left->named->enum_underlying)
			: left,
		right->kind == TK_ENUM
			? MakeFundamentalType(right->named->enum_underlying)
			: right);
	vector<TypePtr> parameters;
	parameters.push_back(common);
	parameters.push_back(common);
	size_t position = ranking.size();
	ranking.push_back(MakeFunctionType(MakeFundamentalType(FT_BOOL),
	                                   parameters, false));
	viable_arity.push_back(2);
	return position;
}

bool SemExprAnalyzer::ResolveOperatorCall(const string& spelling,
                                          vector<SemValue>& operands,
                                          bool member_only,
                                          SemValue& result)
{
	const string op_name = "operator " + spelling;
	vector<OperatorCandidate> candidates;
	CollectOperatorCandidates(op_name, operands, member_only, candidates);
	if (candidates.empty())
		return false;
	vector<ConversionSource> sources;
	for (size_t i = 0; i < operands.size(); i++)
		sources.push_back(MakeConversionSource(operands[i]));
	vector<TypePtr> ranking;
	vector<size_t> viable_arity;
	for (size_t c = 0; c < candidates.size(); c++)
	{
		ranking.push_back(
			CandidateSignature(candidates[c], operands[0]));
		viable_arity.push_back(ranking.back()->parameters.size());
	}
	size_t builtin_pos = AppendBuiltinCandidate(
		spelling, operands, member_only, ranking, viable_arity);
	vector<bool> is_template;
	vector<const FunctionSpecialization*> specs;
	for (size_t c = 0; c < candidates.size(); c++)
	{
		is_template.push_back(candidates[c].spec != 0);
		specs.push_back(candidates[c].spec);
	}
	is_template.resize(ranking.size(), false);
	specs.resize(ranking.size(), (const FunctionSpecialization*)0);
	// Arity filter happens inside SelectBestOverload; a fully
	// non-viable set falls back to the built-in operator. An ambiguous
	// joint ranking is ill-formed (13.3.1.2p3) and propagates.
	vector<ImplicitConversion> conversions;
	size_t winner;
	SpecOverloadOrder order(host_, specs, sources.size());
	try
	{
		winner = SelectBestOverload(ranking, sources, conversions,
		                            &viable_arity, &is_template, &order);
	}
	catch (const NoViableOverloadError&)
	{
		return false;
	}
	if (winner == builtin_pos)
		// The built-in form wins: the caller lowers it directly.
		return false;
	const OperatorCandidate& chosen = candidates[winner];
	const ScopeBinding& binding = *chosen.binding;
	if (chosen.index < binding.fn_deleted.size() &&
	    binding.fn_deleted[chosen.index])
		throw runtime_error("use of deleted " + op_name);
	EMemberAccess access = chosen.index < binding.fn_access.size()
		? binding.fn_access[chosen.index] : MA_PUBLIC;
	host_.CheckMemberAccess(binding.home, access, op_name);
	// PA16: an implicitly declared copy/move assignment synthesizes its
	// definition on first selection.
	if (chosen.is_member && !chosen.spec && binding.name == "operator =")
		if (const NamedTypeInfo* owner_entity =
		        host_.Model().ScopeEntity(binding.owner))
			host_.EnsureAssignSpecial(owner_entity, chosen.index);
	const TypePtr& fn = chosen.declared;
	if (chosen.is_member)
	{
		for (size_t i = 1; i < operands.size(); i++)
			ApplyConversion(operands[i], conversions[i],
			                fn->parameters[i - 1]);
	}
	else
		for (size_t i = 0; i < operands.size(); i++)
			ApplyConversion(operands[i], conversions[i],
			                fn->parameters[i]);
	result = CallResult(fn);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding.owner, binding.name);
	// PA21: a member-template specialization declares its owner class
	// through the template record (its binding owner is the argument
	// alias scope).
	const NamedTypeInfo* member_owner = 0;
	if (chosen.is_member)
		member_owner = chosen.spec && chosen.spec->owner
			? chosen.spec->owner->member_of
			: host_.Model().ScopeEntity(binding.owner);
	callee->type = chosen.is_member
		? ThisAdjustedType(member_owner ? member_owner
		                                : operands[0].type->named, fn)
		: fn;
	callee->entity_scope = binding.owner;
	callee->entity_name = binding.name;
	// A member-template specialization routes through its own entry
	// (keyed on the alias scope) like a namespace-scope one; the
	// object address stays the leading argument.
	callee->is_method = chosen.is_member && !chosen.spec;
	callee->fn_spec = chosen.spec;
	host_.OnSpecializationOdrUsed(chosen.spec);
	if (chosen.index < binding.fn_unwind_no.size() &&
	    binding.fn_unwind_no[chosen.index])
		callee->unwind_no = true;
	if (chosen.index < binding.fn_noexcept_decl.size() &&
	    binding.fn_noexcept_decl[chosen.index])
		callee->noexcept_decl = true;
	result.node->from_operator = true;
	result.node->children.push_back(std::move(callee));
	AppendOperatorOperands(chosen.is_member, member_owner, operands,
	                       *result.node);
	return true;
}

// The resolved operator call's operand children: a member operator
// takes the (possibly base-adjusted) object address first.
void SemExprAnalyzer::AppendOperatorOperands(
	bool is_member, const NamedTypeInfo* member_owner,
	vector<SemValue>& operands, SemNode& call)
{
	if (is_member)
	{
		int hops = member_owner
			? BaseClassDistance(operands[0].type->named, member_owner)
			: 0;
		if (hops > 0)
		{
			SemNodePtr adjusted = MakeSemNode(SN_MEMBER_EXPRESSION);
			adjusted->type = MakeNamedType(TK_CLASS, member_owner);
			adjusted->category = VC_LVALUE;
			adjusted->base_hops = hops;
			adjusted->children.push_back(std::move(operands[0].node));
			operands[0].node = std::move(adjusted);
		}
		call.children.push_back(
			AddressOfObject(std::move(operands[0].node)));
	}
	else
		call.children.push_back(std::move(operands[0].node));
	for (size_t i = 1; i < operands.size(); i++)
		call.children.push_back(std::move(operands[i].node));
}

// Whether a built-in operator form should consult the user-declared
// candidates first.
bool SemExprAnalyzer::OperatorOperand(const SemValue& value)
{
	return value.type->kind == TK_CLASS || value.type->kind == TK_ENUM;
}

bool SemExprAnalyzer::TryBinaryOperator(const string& spelling,
                                        SemValue& lhs, SemValue& rhs,
                                        SemValue& result)
{
	if (!OperatorOperand(lhs) && !OperatorOperand(rhs))
		return false;
	vector<SemValue> operands;
	operands.push_back(std::move(lhs));
	operands.push_back(std::move(rhs));
	if (ResolveOperatorCall(spelling, operands, false, result))
		return true;
	lhs = std::move(operands[0]);
	rhs = std::move(operands[1]);
	return false;
}

bool SemExprAnalyzer::TryUnaryOperator(const string& spelling,
                                       SemValue& operand, bool postfix,
                                       SemValue& result)
{
	if (!OperatorOperand(operand))
		return false;
	vector<SemValue> operands;
	operands.push_back(std::move(operand));
	if (postfix)
	{
		// 13.5.7: the postfix forms take a dummy int argument.
		SemValue dummy;
		dummy.node = MakeSemNode(SN_LITERAL);
		dummy.node->token = "0";
		dummy.node->type = MakeFundamentalType(FT_INT);
		dummy.node->category = VC_PRVALUE;
		dummy.node->has_value = true;
		dummy.node->value = ConstValue(FT_INT, 0);
		dummy.type = dummy.node->type;
		operands.push_back(std::move(dummy));
	}
	if (ResolveOperatorCall(spelling, operands, false, result))
		return true;
	operand = std::move(operands[0]);
	return false;
}

// `object(args)`: the class object's operator() member.
SemValue SemExprAnalyzer::AnalyzeFunctorCall(SemValue object,
                                             const AstExpr& expr)
{
	const ScopeBinding* member = 0;
	Scope* members = host_.Model().MemberScope(object.type->named);
	for (const Scope* link = members; link; link = link->class_base)
		if ((member = FindOwnBinding(*link, "operator ()")))
			break;
	if (!member || member->kind != SB_FUNCTION)
	{
		// 13.3.1.1.2: a conversion function to pointer (or reference)
		// to function supplies a surrogate call: the converted value
		// calls with the arguments.
		TypePtr bare = RemoveTopCv(object.type);
		host_.RequireCompleteType(bare->named);
		const ClassInfo* cls = host_.Classes().Find(bare->named);
		for (const ClassInfo* link = cls; link; link = link->base)
			for (size_t i = 0; i < link->conversions.size(); i++)
			{
				const ClassConversion& conversion = link->conversions[i];
				if (conversion.is_explicit)
					continue;
				TypePtr result = conversion.result;
				if (IsReferenceType(result))
					result = result->target;
				result = RemoveTopCv(result);
				TypePtr function_type;
				if (result->kind == TK_POINTER &&
				    result->target->kind == TK_FUNCTION)
					function_type = result->target;
				else if (result->kind == TK_FUNCTION)
					function_type = result;
				else
					continue;
				ImplicitConversion conv;
				conv.viable = true;
				conv.rank = CR_USER;
				conv.conv_class = link->entity;
				conv.conv_index = (int)i;
				ApplyConversion(object, conv, conversion.result);
				vector<SemValue> args;
				AnalyzeArgumentList(expr.arguments, args);
				CheckCallArguments(function_type, args);
				SemValue value = CallResult(function_type);
				value.node->children.push_back(std::move(object.node));
				for (size_t a = 0; a < args.size(); a++)
					value.node->children.push_back(
						std::move(args[a].node));
				return value;
			}
		throw runtime_error("object is not callable");
	}
	host_.CheckMemberAccess(member->home, member->access, "operator ()");
	return AnalyzeMethodCall(std::move(object), *member, expr.arguments);
}
