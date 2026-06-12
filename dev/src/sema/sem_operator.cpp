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
	switch (type->kind)
	{
	case TK_CLASS:
		for (const NamedTypeInfo* entity = type->named; entity;
		     entity = entity->base_entity)
		{
			const Scope* ns = InnermostNamespace(entity->scope);
			if (ns)
				out.push_back(ns);
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
	size_t count = binding.overloads.size() + 1;
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

// Collects the user-declared candidates of `operator <spelling>` for
// the analyzed operand list.
void SemExprAnalyzer::CollectOperatorCandidates(
	const string& op_name, const vector<SemValue>& operands,
	bool member_only, vector<OperatorCandidate>& out)
{
	set<const void*> seen;
	if (!operands.empty() && operands[0].type->kind == TK_CLASS)
	{
		Scope* members =
			host_.Model().MemberScope(operands[0].type->named);
		for (const Scope* link = members; link;
		     link = link->class_base)
			if (const ScopeBinding* own = FindOwnBinding(*link, op_name))
			{
				AppendBindingOverloads(*own, true, true, out, seen);
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
		AppendBindingOverloads(*visible, false, false, out, seen);
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
			AppendBindingOverloads(*found, false, true, out, seen);
	}
}

// 3.4.2: an unqualified call merges the visible overloads with the
// candidates argument-dependent lookup finds in the arguments'
// associated namespaces (hidden friends included; using-declaration
// imports into an associated namespace are not its own declarations).
SemValue SemExprAnalyzer::AnalyzeAdlCall(const AstExpr& expr,
                                         const string& name,
                                         const ScopeBinding* visible)
{
	vector<SemValue> args;
	vector<ConversionSource> sources;
	for (size_t i = 0; i < expr.arguments.size(); i++)
	{
		args.push_back(Analyze(*expr.arguments[i]));
		sources.push_back(MakeConversionSource(args.back()));
	}
	set<const void*> seen;
	vector<OperatorCandidate> candidates;
	if (visible)
		AppendBindingOverloads(*visible, false, false, candidates, seen);
	vector<const Scope*> namespaces;
	for (size_t i = 0; i < args.size(); i++)
		CollectAssociatedNamespaces(host_.Model(), args[i].type,
		                            namespaces);
	set<const Scope*> visited;
	for (size_t i = 0; i < namespaces.size(); i++)
	{
		if (!visited.insert(namespaces[i]).second)
			continue;
		const ScopeBinding* found = FindOwnBinding(*namespaces[i], name);
		if (found && found->kind == SB_FUNCTION &&
		    found->owner == namespaces[i])
			AppendBindingOverloads(*found, false, true, candidates, seen);
	}
	if (candidates.empty())
		throw runtime_error("undeclared name " + name);
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
	vector<ImplicitConversion> conversions;
	size_t winner = SelectBestOverload(ranking, sources, conversions,
	                                   &min_arity);
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
		SemValue filled =
			Analyze(*binding.fn_defaults[chosen.index][i]);
		CopyInitialize(filled, fn->parameters[i], "default argument");
		args.push_back(std::move(filled));
	}
	SemValue value = CallResult(fn);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding.owner, binding.name);
	callee->type = fn;
	callee->entity_scope = binding.owner;
	callee->entity_name = binding.name;
	if (chosen.index < binding.fn_unwind_no.size() &&
	    binding.fn_unwind_no[chosen.index])
		callee->unwind_no = true;
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

// 13.3.1.2 selection over the collected candidates; false when no
// user-declared candidate is viable (the caller falls back to the
// built-in meaning).
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
		const OperatorCandidate& candidate = candidates[c];
		if (candidate.is_member)
		{
			const NamedTypeInfo* owner_entity =
				host_.Model().ScopeEntity(candidate.binding->owner);
			TypePtr class_type = MakeNamedType(
				TK_CLASS,
				owner_entity ? owner_entity : operands[0].type->named);
			class_type = MakeCvQualifiedType(
				class_type, candidate.declared->is_const,
				candidate.declared->is_volatile);
			bool rvalue_param = candidate.declared->ref_qual == 2 ||
				(candidate.declared->ref_qual == 0 &&
				 operands[0].category != VC_LVALUE);
			vector<TypePtr> parameters;
			parameters.push_back(
				MakeReferenceType(class_type, rvalue_param, true));
			for (size_t i = 0;
			     i < candidate.declared->parameters.size(); i++)
				parameters.push_back(candidate.declared->parameters[i]);
			ranking.push_back(MakeFunctionType(
				candidate.declared->target, parameters,
				candidate.declared->variadic));
		}
		else
			ranking.push_back(candidate.declared);
		viable_arity.push_back(ranking.back()->parameters.size());
	}
	// Arity filter happens inside SelectBestOverload; a fully
	// non-viable set falls back to the built-in operator.
	vector<ImplicitConversion> conversions;
	size_t winner;
	try
	{
		winner = SelectBestOverload(ranking, sources, conversions,
		                            &viable_arity);
	}
	catch (const std::exception&)
	{
		return false;
	}
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
	if (chosen.is_member && binding.name == "operator =")
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
	const NamedTypeInfo* member_owner = chosen.is_member
		? host_.Model().ScopeEntity(binding.owner) : 0;
	callee->type = chosen.is_member
		? ThisAdjustedType(member_owner ? member_owner
		                                : operands[0].type->named, fn)
		: fn;
	callee->entity_scope = binding.owner;
	callee->entity_name = binding.name;
	callee->is_method = chosen.is_member;
	if (chosen.index < binding.fn_unwind_no.size() &&
	    binding.fn_unwind_no[chosen.index])
		callee->unwind_no = true;
	result.node->children.push_back(std::move(callee));
	if (chosen.is_member)
	{
		const NamedTypeInfo* owner_entity = member_owner;
		int hops = owner_entity
			? BaseClassDistance(operands[0].type->named, owner_entity)
			: 0;
		if (hops > 0)
		{
			SemNodePtr adjusted = MakeSemNode(SN_MEMBER_EXPRESSION);
			adjusted->type = MakeNamedType(TK_CLASS, owner_entity);
			adjusted->category = VC_LVALUE;
			adjusted->base_hops = hops;
			adjusted->children.push_back(std::move(operands[0].node));
			operands[0].node = std::move(adjusted);
		}
		result.node->children.push_back(
			AddressOfObject(std::move(operands[0].node)));
	}
	else
		result.node->children.push_back(std::move(operands[0].node));
	for (size_t i = 1; i < operands.size(); i++)
		result.node->children.push_back(std::move(operands[i].node));
	return true;
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
		throw runtime_error("object is not callable");
	host_.CheckMemberAccess(member->home, member->access, "operator ()");
	return AnalyzeMethodCall(std::move(object), *member, expr.arguments);
}
