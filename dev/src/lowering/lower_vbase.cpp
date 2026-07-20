#include "lowering/lower_program.h"

#include <set>
#include <stdexcept>

#include "lowering/lower_function.h"
#include "lowering/lower_types.h"
#include "sema/class_info.h"

using std::runtime_error;

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

// One declared parameter's identity: the name and the declaring scope
// (a same-named local in another scope is a different entity).
typedef std::pair<string, const Scope*> ParamKey;

bool MatchesParamKey(const SemNode& node, const ParamKey& key)
{
	if (key.first.empty() || node.entity_name != key.first)
		return false;
	if (key.second)
		return node.entity_scope == key.second;
	return !node.entity_scope || node.entity_scope->kind != SCOPE_CLASS;
}

// The declared parameter (by index into `names`) an object expression
// bottoms out at, or -1: derefs and projection wrappers peel away.
int BottomParamIndex(const SemNode& node, const vector<ParamKey>& names)
{
	const SemNode* at = &node;
	while (at)
	{
		if (at->kind == SN_ID_EXPRESSION)
		{
			for (size_t i = 0; i < names.size(); i++)
				if (MatchesParamKey(*at, names[i]))
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
bool MentionsParam(const SemNode& node, const ParamKey& key)
{
	if (node.kind == SN_ID_EXPRESSION && MatchesParamKey(node, key))
		return true;
	for (size_t i = 0; i < node.children.size(); i++)
		if (node.children[i] && MentionsParam(*node.children[i], key))
			return true;
	return false;
}

void CollectVBaseDemands(const SemNode& node, const vector<ParamKey>& names,
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
				if (!names[p].first.empty() &&
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
                        bool separate_compilation,
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
		// PA36 host mode: a polymorphic class reads its virtual-base
		// offsets from the installed vtable, so its parameters carry
		// nothing and the signature matches the host ABI exactly.
		if (separate_compilation && cls->is_polymorphic)
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
                         bool separate_compilation,
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
	AppendParamCarried(fn_type, is_method, 0, separate_compilation, out);
}

const vector<HiddenParam>& HiddenSignatureParams(LowFunctionInfo& info,
                                                 bool separate_compilation)
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
		vector<ParamKey> names(
			info.type->parameters.size(),
			ParamKey(string(), (const Scope*)0));
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
			{
				names[at].first = child.name;
				names[at].second = child.entity_scope;
				at++;
			}
		}
		for (size_t c = 0; c < info.definition->children.size(); c++)
			CollectVBaseDemands(*info.definition->children[c], names,
			                    demands);
		filter = &demands;
	}
	AppendParamCarried(info.type, info.is_method, filter,
	                   separate_compilation, info.hidden);
	return info.hidden;
}

// --- PA27 hidden vbase-pointer machinery -----------------------------------

// The id-expression an object/argument expression bottoms out at,
// peeling dereferences, casts, and unnamed base projections ("this"
// included). A named member access (a field read) breaks the
// identity: the value no longer designates the parameter's subobject.
static const SemNode* BottomIdExpression(const SemNode& node)
{
	const SemNode* at = &node;
	while (at)
	{
		switch (at->kind)
		{
		case SN_ID_EXPRESSION:
			return at;
		case SN_UNARY_EXPRESSION:
		case SN_CAST_EXPRESSION:
			break;
		case SN_MEMBER_EXPRESSION:
			if (!at->name.empty())
				return 0;
			break;
		default:
			return 0;
		}
		if (at->children.empty())
			return 0;
		at = at->children[0].get();
	}
	return 0;
}

// Emits the carried-pointer slots of one declared parameter: hidden
// arguments store first, the remaining table entries reconstruct from
// static complete-object offsets in post-order.
void FunctionLowerer::EmitParamVBasePointers(size_t param_pos,
                                             size_t type_index,
                                             const SemNode& child)
{
	// The hidden `this` parameter carries no __pvbptr row; base
	// entries and member functions ride their own __vbptrN map.
	if (child.name == "this")
		return;
	bool collapsed = false;
	const ClassInfo* cls = ParamVBaseClass(child.type, collapsed);
	if (!cls)
		return;
	// PA36 host mode: polymorphic classes carry nothing; their access
	// paths read offsets from the installed vtable, which stays right
	// when the parameter views a non-complete object.
	if (program_.SeparateCompilation() && cls->is_polymorphic)
		return;
	VBaseParamMap& map = vbase_params_[params_[param_pos].low_name];
	map.cls = cls;
	map.scope = child.entity_scope;
	for (size_t h = 0; h < hidden_params_.size(); h++)
	{
		const HiddenParam& hp = hidden_params_[h];
		if (hp.kind == HP_PVBPTR && hp.param_index == type_index)
			map.carried[hp.vbase_index] = "%" + hp.low_name;
	}
	map.slots = !collapsed && IsReferenceType(child.type);
	if (!map.slots)
		return;
	// Slots declare in table order; the stores run post-order (a
	// shared base's own bases before it): carried entries store their
	// hidden argument, the rest reconstruct from static
	// complete-object offsets.
	const string& base_name = params_[param_pos].low_name;
	vector<string> slots(cls->vbases.size());
	for (size_t k = 0; k < cls->vbases.size(); k++)
		slots[k] = AddSlot(child.entity_scope,
		                   base_name + "__pvb" + to_string(k), "ptr");
	vector<size_t> order;
	PostOrderVBases(*cls, order);
	for (size_t i = 0; i < order.size(); i++)
	{
		size_t k = order[i];
		std::map<size_t, string>::const_iterator carried =
			map.carried.find(k);
		if (carried != map.carried.end())
		{
			Emit("store ptr " + carried->second + ", $" + slots[k]);
			continue;
		}
		string address = NewTemp();
		Emit(address + " = index i8 %" + base_name + ", " +
		     to_string(cls->vbases[k].offset));
		Emit("store ptr " + address + ", $" + slots[k]);
	}
}

// The construction-table operand of a polymorphic base entry's __vtt
// argument: a base entry slices its own table, a complete-object
// entry addresses the class's VTT global at the base's sub-VTT.
string FunctionLowerer::SupplyVttArgument(const ClassInfo& callee_cls)
{
	const ClassInfo* own = program_.MethodClass(def_.type);
	if (!own)
		throw runtime_error("construction-table argument outside a "
		                    "constructor context");
	size_t index = program_.SubVttIndex(*own, callee_cls);
	if (!vtt_param_.empty())
	{
		string sliced = NewTemp();
		Emit(sliced + " = index i8 " + vtt_param_ + ", " +
		     to_string(8 * index));
		return sliced;
	}
	string table = NewTemp();
	Emit(table + " = addr " + program_.VttRef(own));
	string sliced = NewTemp();
	Emit(sliced + " = index i8 " + table + ", " + to_string(8 * index));
	return sliced;
}

// The current function's own address of one shared virtual-base
// subobject: base entries forward their hidden row; complete-object
// entries project the static offset from `this`.
string FunctionLowerer::OwnVBaseEntryAddress(const ClassInfo* entry_cls)
{
	map<string, VBaseParamMap>::const_iterator found =
		vbase_params_.find("this");
	if (found != vbase_params_.end())
		for (size_t i = 0; i < found->second.cls->vbases.size(); i++)
			if (found->second.cls->vbases[i].cls == entry_cls)
			{
				map<size_t, string>::const_iterator carried =
					found->second.carried.find(i);
				if (carried != found->second.carried.end())
					return carried->second;
				break;
			}
	const ClassInfo* own = program_.MethodClass(def_.type);
	const ClassVBase* row = own ? FindClassVBase(*own, entry_cls) : 0;
	if (!row)
		throw std::runtime_error(
			"hidden virtual-base pointer has no source subobject");
	string self = NewTemp();
	Emit(self + " = load ptr $this");
	string address = NewTemp();
	Emit(address + " = index i8 " + self + ", " +
	     to_string(row->offset));
	return address;
}

// The subobject address of `entry_cls` for one hidden call argument,
// anchored on the (already lowered) argument expression.
string FunctionLowerer::SupplyVBaseEntry(const ClassInfo& param_cls,
                                         const ClassInfo* entry_cls,
                                         const SemNode* arg_node,
                                         const string& arg_text)
{
	const SemNode* bottom = arg_node ? BottomIdExpression(*arg_node) : 0;
	if (bottom)
	{
		if (bottom->entity_name == "this")
			return OwnVBaseEntryAddress(entry_cls);
		map<string, VBaseParamMap>::const_iterator found =
			vbase_params_.find(bottom->entity_name);
		if (found != vbase_params_.end() && found->second.cls &&
		    (!found->second.scope ||
		     found->second.scope == bottom->entity_scope))
		{
			const VBaseParamMap& map = found->second;
			const ClassVBase* row = FindClassVBase(*map.cls, entry_cls);
			if (row)
			{
				size_t index = row - &map.cls->vbases[0];
				if (map.slots)
				{
					string loaded = NewTemp();
					Emit(loaded + " = load ptr $" +
					     SlotRef(bottom->entity_scope,
					             bottom->entity_name + "__pvb" +
					                 to_string(index)));
					return loaded;
				}
				std::map<size_t, string>::const_iterator carried =
					map.carried.find(index);
				if (carried != map.carried.end())
					return carried->second;
			}
		}
		// A named complete object: the entry sits at its static
		// complete-object offset (the declared class knows the layout
		// even when the entry is deeper than the parameter's view). A
		// pointer variable anchors on its loaded value.
		const ScopeBinding* binding = EntityBinding(*bottom);
		if (binding && binding->kind == SB_VARIABLE &&
		    !IsReferenceType(binding->type))
		{
			TypePtr declared = RemoveTopCv(binding->type);
			bool through_pointer = declared->kind == TK_POINTER;
			if (through_pointer)
				declared = RemoveTopCv(declared->target);
			const ClassInfo* record = declared->kind == TK_CLASS
				? declared->named->class_record : 0;
			unsigned long long offset = 0;
			if (record &&
			    CompleteObjectOffset(*record, entry_cls->entity, offset))
			{
				// The lowered argument doubles as the anchor when it
				// went through unadjusted; otherwise re-address the
				// named object. One call's hidden row shares the
				// anchor (and a pointer's single load).
				string anchor;
				map<const SemNode*, string>::const_iterator seen =
					supply_anchors_.find(arg_node);
				if (seen != supply_anchors_.end())
					anchor = seen->second;
				else
				{
					anchor =
						arg_node == bottom &&
						(through_pointer || record == &param_cls)
							? arg_text : LowerAddressExpr(*bottom);
					if (through_pointer)
					{
						string loaded = NewTemp();
						Emit(loaded + " = load ptr " + anchor);
						anchor = loaded;
					}
					supply_anchors_[arg_node] = anchor;
				}
				string address = NewTemp();
				Emit(address + " = index i8 " + anchor + ", " +
				     to_string(offset));
				return address;
			}
		}
	}
	// Fallback through the parameter's own class: a polymorphic view
	// reads the offset from the vpointer header; otherwise the static
	// complete-object layout stands in.
	const ClassVBase* row = FindClassVBase(param_cls, entry_cls);
	if (!row)
		throw std::runtime_error(
			"hidden virtual-base pointer has no argument subobject");
	if (param_cls.is_polymorphic)
	{
		size_t index = row - &param_cls.vbases[0];
		string vpointer = NewTemp();
		Emit(vpointer + " = load ptr " + arg_text);
		string slot = NewTemp();
		Emit(slot + " = index i8 " + vpointer + ", -" +
		     to_string(24 + 8 * index));
		string offset = NewTemp();
		Emit(offset + " = load i64 " + slot);
		string address = NewTemp();
		Emit(address + " = index i8 " + arg_text + ", " + offset);
		return address;
	}
	string address = NewTemp();
	Emit(address + " = index i8 " + arg_text + ", " +
	     to_string(row->offset));
	return address;
}

// Appends the callee's hidden trailing arguments (see pa27/plan.md).
void FunctionLowerer::AppendHiddenArguments(
	const vector<HiddenParam>& hidden,
	const vector<const SemNode*>& arg_nodes,
	const vector<string>& arg_texts, const SemNode* object_node,
	const string& object_text, string& arguments)
{
	supply_anchors_.clear();
	for (size_t h = 0; h < hidden.size(); h++)
	{
		const HiddenParam& hp = hidden[h];
		string value;
		switch (hp.kind)
		{
		case HP_VTT:
			value = SupplyVttArgument(*hp.cls);
			break;
		case HP_VBPTR:
		{
			const ClassInfo* entry = hp.cls->vbases[hp.vbase_index].cls;
			const SemNode* bottom =
				object_node ? BottomIdExpression(*object_node) : 0;
			if (!object_node || (bottom && bottom->entity_name == "this"))
				value = OwnVBaseEntryAddress(entry);
			else
				value = SupplyVBaseEntry(*hp.cls, entry, object_node,
				                         object_text);
			break;
		}
		case HP_PVBPTR:
		{
			const ClassInfo* entry = hp.cls->vbases[hp.vbase_index].cls;
			const SemNode* arg = hp.param_index < arg_nodes.size()
				? arg_nodes[hp.param_index] : 0;
			string text = hp.param_index < arg_texts.size()
				? arg_texts[hp.param_index] : string();
			value = SupplyVBaseEntry(*hp.cls, entry, arg, text);
			break;
		}
		}
		arguments += (arguments.empty() ? "" : ", ") + value;
	}
}

// PA27: the subobject address behind a virtual-edge member path. The
// carrier virtual base resolves per context: a non-polymorphic static
// class projects the static complete-object offset in one collapsed
// index; a polymorphic one loads the offset from the vpointer header
// (entry k at addresspoint - 24 - 8k), then walks the remaining
// virtual hops inside the carrier. `with_projection` appends the final
// non-virtual remainder projection (member access always projects;
// bare base adjustments only when the remainder is non-zero).
string FunctionLowerer::VBaseCarrierAddress(const SemNode& node,
                                            bool with_projection)
{
	return VBaseSubobjectAddress(
		*node.children[0], node.vbase_index, node.base_offset,
		node.entity_scope ? node.entity_scope->entity : 0,
		with_projection);
}

string FunctionLowerer::VBaseSubobjectAddress(const SemNode& object,
                                              size_t vbase_index,
                                              unsigned long long remainder,
                                              const NamedTypeInfo* owner,
                                              bool with_projection)
{
	TypePtr object_type = RemoveTopCv(object.type);
	if (IsReferenceType(object_type))
		object_type = RemoveTopCv(object_type->target);
	const ClassInfo* record = object_type->named->class_record;
	const ClassVBase& carrier = record->vbases[vbase_index];
	// A parameter (or `this`) with carried pointers supplies the
	// carrier directly. The node's index counts the object's static
	// class's table; the carried row lives in the parameter's own
	// table (the orders can differ across a base view), so the carrier
	// class remaps before any slot or register is touched.
	const SemNode* bottom = BottomIdExpression(object);
	if (bottom)
	{
		map<string, VBaseParamMap>::const_iterator found =
			vbase_params_.find(bottom->entity_name);
		const ClassVBase* prow =
			found != vbase_params_.end() && found->second.cls &&
			(!found->second.scope ||
			 found->second.scope == bottom->entity_scope)
				? FindClassVBase(*found->second.cls, carrier.cls) : 0;
		if (prow)
		{
			const VBaseParamMap& param = found->second;
			size_t pindex = (size_t)(prow - &param.cls->vbases[0]);
			string address;
			if (param.slots)
			{
				address = NewTemp();
				Emit(address + " = load ptr $" +
				     SlotRef(bottom->entity_scope,
				             bottom->entity_name + "__pvb" +
				                 to_string(pindex)));
			}
			else
			{
				std::map<size_t, string>::const_iterator carried =
					param.carried.find(pindex);
				if (carried == param.carried.end())
					throw runtime_error(
						"virtual-base carrier is not carried");
				address = carried->second;
			}
			return VBaseRemainderHops(
				address, *carrier.cls,
				owner,
				remainder, with_projection);
		}
	}
	string base = LowerAddressExpr(object);
	if (!record->is_polymorphic)
	{
		// An unknown-provenance reference (a call result) reconstructs
		// the carrier from the static complete layout, then hops like
		// a carried pointer.
		if (object.kind == SN_CALL_EXPRESSION)
		{
			string located = NewTemp();
			Emit(located + " = index i8 " + base + ", " +
			     to_string(carrier.offset));
			return VBaseRemainderHops(
				located, *carrier.cls,
				owner,
				remainder, with_projection);
		}
		// Static complete-object collapse: one projection covering the
		// carrier and every hop inside it.
		unsigned long long total = carrier.offset + remainder;
		if (!with_projection && total == 0)
			return base;
		string projected = NewTemp();
		Emit(projected + " = index i8 [projection=base_subobject] " +
		     base + ", " + to_string(total));
		return projected;
	}
	// Dynamic carrier: the vbase offset lives in the vtable header.
	return VBaseRemainderHops(DynamicVBaseAddress(base, vbase_index),
	                          *carrier.cls,
	                          owner,
	                          remainder, with_projection);
}

string FunctionLowerer::DynamicVBaseAddress(const string& address,
                                            size_t vbase_index)
{
	string vpointer = NewTemp();
	Emit(vpointer + " = load ptr " + address);
	string slot = NewTemp();
	Emit(slot + " = index i8 " + vpointer + ", -" +
	     to_string(24 + 8 * (unsigned long long)vbase_index));
	string offset = NewTemp();
	Emit(offset + " = load i64 " + slot);
	string at = NewTemp();
	Emit(at + " = index i8 " + address + ", " + offset);
	return at;
}

// The hops from a carrier virtual base down to the owner subobject:
// one projection per further virtual edge (each a static offset inside
// the previous carrier's complete object), then the non-virtual
// remainder projection.
string FunctionLowerer::VBaseRemainderHops(const string& base_in,
                                           const ClassInfo& carrier,
                                           const NamedTypeInfo* owner,
                                           unsigned long long remainder,
                                           bool with_projection)
{
	string base = base_in;
	const ClassInfo* at = &carrier;
	unsigned long long nv_offset = remainder;
	bool hopped_any = false;
	while (owner)
	{
		int hops = 0;
		unsigned long long nv = 0;
		if (at->entity == owner ||
		    BaseSubobjectPath(at->entity, owner, hops, nv) == BP_UNIQUE)
		{
			nv_offset = nv;
			break;
		}
		size_t index = 0;
		unsigned long long inner = 0;
		if (!VirtualBasePath(*at, owner, index, inner))
			break;
		string hopped = NewTemp();
		Emit(hopped + " = index i8 [projection=base_subobject] " + base +
		     ", " + to_string(at->vbases[index].offset));
		base = hopped;
		at = at->vbases[index].cls;
		nv_offset = inner;
		hopped_any = true;
	}
	// A bare adjustment suppresses a zero remainder only after a hop
	// already moved the address (the reference keeps one projection).
	if (!with_projection && nv_offset == 0 && hopped_any)
		return base;
	string projected = NewTemp();
	Emit(projected + " = index i8 [projection=base_subobject] " + base +
	     ", " + to_string(nv_offset));
	return projected;
}

// The hidden trailing arguments of one lowered call: direct callees
// follow their registry signature, indirect ones the type-only rule.
void FunctionLowerer::AppendCallHiddenArguments(
	const SemNode& node, const SemNode& callee, const TypePtr& fn_type,
	bool direct, bool pm_call, const vector<const SemNode*>& arg_nodes,
	const vector<string>& arg_texts, const string& object_text,
	string& arguments)
{
	vector<HiddenParam> type_hidden;
	const vector<HiddenParam>* hidden = 0;
	if (direct)
		hidden = &HiddenSignatureParams(program_.CalleeEntryInfo(callee),
		                                program_.SeparateCompilation());
	else
	{
		HiddenParamsForType(
			pm_call ? MemberPointerCallSignature(fn_type) : fn_type,
			pm_call, program_.SeparateCompilation(), type_hidden);
		hidden = &type_hidden;
	}
	if (hidden->empty())
		return;
	AppendHiddenArguments(
		*hidden, arg_nodes, arg_texts,
		node.children.size() > 1 && (callee.is_method || pm_call)
			? (pm_call ? callee.children[0].get()
			           : node.children[1].get())
			: 0,
		object_text, arguments);
}

// Shared virtual-base construction/destruction belongs to the
// complete-object entries; base entries and the deleting entry drop
// the marked actions but keep the constructor callee demanded (the
// reference emits the definitions beside the base entry).
bool FunctionLowerer::SkipVBaseAction(const SemNode& node)
{
	if (!node.vbase_action ||
	    (info_.special_code != "C2" && info_.special_code != "D2" &&
	     info_.special_code != "D0"))
		return false;
	if (node.kind == SN_CONSTRUCTOR_ACTION && !node.trivial_init &&
	    !node.children.empty() && !node.children[0]->children.empty())
		program_.MemberFunctionRef(*node.children[0]->children[0]);
	return true;
}

// PA27: whether the (already lowered) callee body carries a
// null-guarded displaced-base adjustment; callers under cleanups wrap
// such calls in a lazy dispatch region even when the sema unwind fact
// stayed non-throwing.
bool LowerProgram::CalleeGuardedBody(const SemNode& callee)
{
	if (callee.kind != SN_CALLEE || !callee.entity_scope)
		return false;
	return CalleeEntryInfo(callee).guarded_body;
}

// PA27: the registry entry a direct call resolves to (for the hidden
// vbase-pointer signature), without demanding it.
LowFunctionInfo& LowerProgram::CalleeEntryInfo(const SemNode& callee)
{
	if (callee.is_method || callee.special != SF_NONE)
	{
		const char* code = "";
		switch (callee.special)
		{
		case SF_CONSTRUCTOR: code = "C1"; break;
		case SF_CONSTRUCTOR_BASE: code = "C2"; break;
		case SF_DESTRUCTOR: code = "D1"; break;
		case SF_DESTRUCTOR_BASE: code = "D2"; break;
		default:
			break;
		}
		return MemberFunctionEntry(callee.entity_scope,
		                           callee.entity_name, callee.type, code);
	}
	return FunctionEntry(callee.entity_scope, callee.entity_name,
	                     callee.type, callee.fn_spec);
}

// PA27 comdat pairing context: a weak constructor/destructor of a
// virtual-base class pairs every user-provided subobject entry; a
// polymorphic one pairs the entries on its primary chain.
bool LowerProgram::ContextPairsSubobjectEntry(const ClassInfo* cls)
{
	if (!lowering_context_ || !lowering_context_->is_method ||
	    !lowering_context_->weak ||
	    lowering_context_->special_code.empty())
		return false;
	const ClassInfo* ctx_cls = MethodClass(lowering_context_->type);
	if (!ctx_cls)
		return false;
	if (ClassHasVBases(*ctx_cls))
		return true;
	if (ctx_cls->is_polymorphic)
		for (const ClassInfo* at = ctx_cls->base; at; at = at->base)
			if (at == cls)
				return true;
	return false;
}

// Aliases follow the definitions without a separating blank line. The
// base entry printing its own (differing) body suppresses the
// complete entry's alias (PA27).
void LowerProgram::AppendAliasSection(vector<string>& section)
{
	for (size_t i = 0; i < functions_.size(); i++)
	{
		const LowFunctionInfo& info = functions_[i];
		if (!info.defined || info.body_text.empty() ||
		    (info.weak && !info.used) || info.alias_object.empty())
			continue;
		if (info.special_code == "C1" || info.special_code == "D1")
		{
			LowFunctionInfo& base_entry = MemberFunctionEntry(
				info.scope, info.name, info.type,
				info.special_code == "C1" ? "C2" : "D2");
			if (base_entry.defined && base_entry.used &&
			    !base_entry.body_text.empty())
				continue;
		}
		section.push_back("alias object " + info.alias_object +
		                  " = @" + info.low_name);
	}
}

// SN_VPOINTER_STORE: the constructor/destructor stores the address of
// the class's first vtable function slot (vtable + 16, past the
// offset-to-top and RTTI entries) into the object's vpointer.
void FunctionLowerer::LowerVPointerStore(const SemNode& node)
{
	const ClassInfo* cls = RemoveTopCv(node.type)->named->class_record;
	if (!cls)
		throw runtime_error("vpointer store without a class record");
	// PA27: base entries of a polymorphic virtual-base class install
	// their vpointers from the construction table.
	bool from_vtt = !vtt_param_.empty();
	string self = LowerValueExpr(*node.children[0]).text;
	if (from_vtt)
	{
		string loaded = NewTemp();
		Emit(loaded + " = load ptr " + vtt_param_);
		Emit("store ptr " + loaded + ", " + self);
	}
	else
	{
		string table = NewTemp();
		Emit(table + " = addr " + program_.VTableRef(cls));
		string entry = NewTemp();
		Emit(entry + " = index i8 " + table + ", " +
		     to_string(LowerProgram::VTableAddressPoint(cls)));
		Emit("store ptr " + entry + ", " + self);
	}
	if (cls->views.empty())
		return;
	// The non-primary vpointer locations (outermost views), each a
	// fresh projection from the object address.
	program_.VTableGroupRef(cls);
	size_t ordinal = 0;
	for (size_t v = 0; v < cls->views.size(); v++)
	{
		const ClassView& view = cls->views[v];
		if (!view.outermost)
			continue;
		string base = LowerValueExpr(*node.children[0]).text;
		string target = NewTemp();
		Emit(target + " = index i8 [projection=base_subobject] " + base +
		     ", " + to_string(view.offset));
		if (from_vtt)
		{
			string slot = NewTemp();
			Emit(slot + " = index i8 " + vtt_param_ + ", " +
			     to_string(8 * LowerProgram::OwnViewVttIndex(*cls,
			                                                 ordinal)));
			string loaded = NewTemp();
			Emit(loaded + " = load ptr " + slot);
			Emit("store ptr " + loaded + ", " + target);
		}
		else
		{
			string table = NewTemp();
			Emit(table + " = addr " + program_.ViewVTableRef(cls, v));
			unsigned long long point =
				LowerProgram::ViewGroupAddressPoint(*cls, view);
			string value = table;
			if (point)
			{
				value = NewTemp();
				Emit(value + " = index i8 " + table + ", " +
				     to_string(point));
			}
			Emit("store ptr " + value + ", " + target);
		}
		ordinal++;
	}
}

// The declared argument row of one lowered call plus its hidden
// trailing vbase pointers (member-pointer calls count their prepended
// object parameter).
void FunctionLowerer::LowerCallArgumentRow(
	const SemNode& node, const SemNode& callee, const TypePtr& fn_type,
	bool direct, bool pm_call, string& object_text, string& arguments)
{
	vector<const SemNode*> arg_nodes(
		fn_type->parameters.size() + (pm_call ? 1 : 0), 0);
	vector<string> arg_texts(arg_nodes.size());
	if (pm_call && !arg_nodes.empty())
	{
		arg_nodes[0] = callee.children[0].get();
		arg_texts[0] = object_text;
	}
	for (size_t i = 1; i < node.children.size(); i++)
	{
		TypePtr param = i - 1 < fn_type->parameters.size()
			? fn_type->parameters[i - 1] : TypePtr();
		string text = LowerCallArgument(*node.children[i], param);
		if (i == 1 && !pm_call)
			object_text = text;
		size_t param_at = i - 1 + (pm_call ? 1 : 0);
		if (param_at < arg_nodes.size())
		{
			arg_nodes[param_at] = node.children[i].get();
			arg_texts[param_at] = text;
		}
		arguments += (i > 1 || pm_call ? ", " : "") + text;
	}
	AppendCallHiddenArguments(node, callee, fn_type, direct, pm_call,
	                          arg_nodes, arg_texts, object_text,
	                          arguments);
}
