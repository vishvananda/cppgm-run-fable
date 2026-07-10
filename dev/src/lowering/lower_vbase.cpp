#include "lowering/lower_program.h"

#include <set>

#include "sema/class_info.h"

using std::to_string;

// PA27 hidden vbase-pointer ABI (see pa27/plan.md). Base entries of
// classes with virtual bases carry `__vtt` (polymorphic ones) and one
// `__vbptrN` per vbase-table entry; member functions of non-
// polymorphic virtual-base classes carry the same `__vbptrN` row; and
// reference/pointer parameters of virtual-base classes carry trailing
// `__pvbptrK` subobject addresses. Template specializations keep only
// the entries their instantiated bodies demand; the rest reconstruct
// from static complete-object offsets.

namespace {

// The class record behind a (possibly cv-qualified) class type.
const ClassInfo* TypeClassRecord(const TypePtr& type)
{
	TypePtr bare = RemoveTopCv(type);
	if (bare->kind != TK_CLASS || !bare->named)
		return 0;
	return bare->named->class_record;
}

}  // namespace

const ClassInfo* ParamVBaseClass(const TypePtr& param, bool& collapsed)
{
	collapsed = false;
	TypePtr bare = RemoveTopCv(param);
	if (IsReferenceType(bare))
		bare = RemoveTopCv(bare->target);
	if (bare->kind == TK_POINTER)
	{
		collapsed = true;
		bare = RemoveTopCv(bare->target);
	}
	const ClassInfo* record = TypeClassRecord(bare);
	if (!record || !ClassHasVBases(*record))
		return 0;
	return record;
}

void ParamCarriedEntries(const ClassInfo& cls, bool collapsed,
                         vector<size_t>& out)
{
	out.clear();
	for (size_t i = 0; i < cls.vbases.size(); i++)
	{
		if (collapsed)
		{
			// An entry inside a direct virtual base collapses into the
			// carried direct-base pointer (its position inside that
			// base's complete object is static).
			bool inner = false;
			for (size_t b = 0; !inner && b < cls.direct_bases.size(); b++)
				if (cls.direct_bases[b].is_virtual &&
				    FindClassVBase(*cls.direct_bases[b].cls,
				                   cls.vbases[i].cls))
					inner = true;
			if (inner)
				continue;
		}
		out.push_back(i);
	}
}

namespace {

// --- template demand analysis ------------------------------------------

// The demands an instantiated body places on its parameters' carried
// entries: a virtual-edge member access through a parameter demands
// the carrier entry; passing the parameter as a call or constructor
// argument demands the parameter's full set.
struct VBaseDemands
{
	std::set<std::pair<size_t, size_t>> entries;  // (param, table index)
	std::set<size_t> full;                        // parameter index
};

// The declared parameter (by index into `names`) an object expression
// bottoms out at, or -1: derefs and projection wrappers peel away.
int BottomParamIndex(const SemNode& node, const vector<string>& names)
{
	const SemNode* at = &node;
	while (at)
	{
		if (at->kind == SN_ID_EXPRESSION)
		{
			if (at->entity_scope &&
			    at->entity_scope->kind == SCOPE_CLASS)
				return -1;
			for (size_t i = 0; i < names.size(); i++)
				if (!names[i].empty() && at->entity_name == names[i])
					return (int)i;
			return -1;
		}
		if (at->children.empty())
			return -1;
		at = at->children[0].get();
	}
	return -1;
}

// Whether the subtree mentions the parameter at all (argument
// positions demand the full set).
bool MentionsParam(const SemNode& node, const string& name)
{
	if (node.kind == SN_ID_EXPRESSION && node.entity_name == name &&
	    (!node.entity_scope || node.entity_scope->kind != SCOPE_CLASS))
		return true;
	for (size_t i = 0; i < node.children.size(); i++)
		if (node.children[i] && MentionsParam(*node.children[i], name))
			return true;
	return false;
}

void CollectVBaseDemands(const SemNode& node, const vector<string>& names,
                         VBaseDemands& out)
{
	if (node.kind == SN_MEMBER_EXPRESSION && node.vbase_index >= 0 &&
	    !node.children.empty())
	{
		int param = BottomParamIndex(*node.children[0], names);
		if (param >= 0)
			out.entries.insert(
				std::make_pair((size_t)param, (size_t)node.vbase_index));
	}
	if (node.kind == SN_CALL_EXPRESSION)
	{
		for (size_t a = 1; a < node.children.size(); a++)
		{
			if (!node.children[a])
				continue;
			for (size_t p = 0; p < names.size(); p++)
				if (!names[p].empty() &&
				    MentionsParam(*node.children[a], names[p]))
					out.full.insert(p);
		}
	}
	for (size_t i = 0; i < node.children.size(); i++)
		if (node.children[i])
			CollectVBaseDemands(*node.children[i], names, out);
}

// PA18: whether the entity is (or is nested inside) an instantiated
// class-template specialization (mirrors the sema-side walk).
bool ScopeInInstantiation(const Scope* scope)
{
	for (const Scope* at = scope; at; at = at->parent)
		if (at->entity && at->entity->spec_template &&
		    !at->entity->is_template_anchor)
			return true;
	return false;
}

void AppendParamCarried(const TypePtr& fn_type, bool is_method,
                        const VBaseDemands* demands,
                        vector<HiddenParam>& out)
{
	size_t counter = 0;
	size_t first = is_method ? 1 : 0;
	for (size_t p = first; p < fn_type->parameters.size(); p++)
	{
		bool collapsed = false;
		const ClassInfo* cls =
			ParamVBaseClass(fn_type->parameters[p], collapsed);
		if (!cls)
			continue;
		vector<size_t> entries;
		ParamCarriedEntries(*cls, collapsed, entries);
		for (size_t e = 0; e < entries.size(); e++)
		{
			if (demands && !demands->full.count(p) &&
			    !demands->entries.count(std::make_pair(p, entries[e])))
				continue;
			HiddenParam hidden;
			hidden.kind = HP_PVBPTR;
			hidden.param_index = p;
			hidden.vbase_index = entries[e];
			hidden.cls = cls;
			hidden.low_name = "__pvbptr" + to_string(counter++);
			out.push_back(hidden);
		}
	}
}

}  // namespace

void HiddenParamsForType(const TypePtr& fn_type, bool is_method,
                         vector<HiddenParam>& out)
{
	out.clear();
	if (is_method && !fn_type->parameters.empty())
	{
		const ClassInfo* method_class =
			fn_type->parameters[0]->kind == TK_POINTER
				? TypeClassRecord(fn_type->parameters[0]->target) : 0;
		if (method_class && ClassHasVBases(*method_class) &&
		    !method_class->is_polymorphic)
			for (size_t i = 0; i < method_class->vbases.size(); i++)
			{
				HiddenParam hidden;
				hidden.kind = HP_VBPTR;
				hidden.vbase_index = i;
				hidden.cls = method_class;
				hidden.low_name = "__vbptr" + to_string(i);
				out.push_back(hidden);
			}
	}
	AppendParamCarried(fn_type, is_method, 0, out);
}

const vector<HiddenParam>& HiddenSignatureParams(LowFunctionInfo& info)
{
	if (info.hidden_ready)
		return info.hidden;
	info.hidden_ready = true;
	const ClassInfo* method_class =
		info.is_method && info.scope && info.scope->entity
			? info.scope->entity->class_record : 0;
	bool base_entry =
		info.special_code == "C2" || info.special_code == "D2";
	if (method_class && ClassHasVBases(*method_class))
	{
		if (base_entry)
		{
			if (method_class->is_polymorphic)
			{
				HiddenParam vtt;
				vtt.kind = HP_VTT;
				vtt.cls = method_class;
				vtt.low_name = "__vtt";
				info.hidden.push_back(vtt);
			}
			for (size_t i = 0; i < method_class->vbases.size(); i++)
			{
				HiddenParam hidden;
				hidden.kind = HP_VBPTR;
				hidden.vbase_index = i;
				hidden.cls = method_class;
				hidden.low_name = "__vbptr" + to_string(i);
				info.hidden.push_back(hidden);
			}
		}
		else if (info.special_code.empty() &&
		         !method_class->is_polymorphic)
		{
			// Ordinary member functions of a non-polymorphic
			// virtual-base class carry the table; polymorphic classes
			// read the offsets from the vpointer instead.
			for (size_t i = 0; i < method_class->vbases.size(); i++)
			{
				HiddenParam hidden;
				hidden.kind = HP_VBPTR;
				hidden.vbase_index = i;
				hidden.cls = method_class;
				hidden.low_name = "__vbptr" + to_string(i);
				info.hidden.push_back(hidden);
			}
		}
	}
	// Synthesized special members take no parameter pointers: their
	// `other` reconstructs from static complete-object offsets.
	if (info.definition && info.definition->synthesized)
		return info.hidden;
	// Template specializations keep only the demanded entries.
	VBaseDemands demands;
	const VBaseDemands* filter = 0;
	bool instantiated = info.fn_spec != 0 ||
		(info.definition && info.scope &&
		 ScopeInInstantiation(info.scope));
	if (instantiated && info.definition)
	{
		vector<string> names(info.type->parameters.size());
		size_t first = info.is_method ? 1 : 0;
		size_t at = first;
		for (size_t c = 0; c < info.definition->children.size(); c++)
		{
			const SemNode& child = *info.definition->children[c];
			if (child.kind != SN_PARAMETER)
				continue;
			if (child.name == "this")
				continue;
			if (at < names.size())
				names[at++] = child.name;
		}
		for (size_t c = 0; c < info.definition->children.size(); c++)
			CollectVBaseDemands(*info.definition->children[c], names,
			                    demands);
		filter = &demands;
	}
	AppendParamCarried(info.type, info.is_method, filter, info.hidden);
	return info.hidden;
}
