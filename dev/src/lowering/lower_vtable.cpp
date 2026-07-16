#include "lowering/lower_program.h"

#include <stdexcept>

#include "lowering/lower_name.h"
#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

// PA17 vtable and RTTI-lite emission. Vtable globals are demanded by
// the vpointer stores lowered into constructor/destructor bodies and
// by key-function definitions; rendering a vtable demands its slot
// functions (including the deleting destructor entries) and the RTTI
// chain of the class and its bases.

const ClassInfo* LowerProgram::MethodClass(const TypePtr& adjusted) const
{
	if (adjusted->parameters.empty() ||
	    adjusted->parameters[0]->kind != TK_POINTER)
		return 0;
	TypePtr target = RemoveTopCv(adjusted->parameters[0]->target);
	return target->kind == TK_CLASS ? target->named->class_record : 0;
}

namespace {

// The readable name tail of a fundamental type ("long int" ->
// "long_int"); RTTI and exception-object scaffolding names use it.
string FundamentalNameTail(const TypePtr& type)
{
	string display = DescribeType(type);
	string tail;
	for (size_t i = 0; i < display.size(); i++)
	{
		char c = display[i];
		tail += (c == ' ') ? '_' : c;
	}
	return tail;
}

// A specialization whose arguments all sanitize (type arguments that
// are fundamentals, enumerations, or such classes; value arguments)
// keeps the readable class-keyed RTTI naming; any other argument
// switches the record to its encoding-keyed name.
bool SimpleRttiTemplateArg(const TemplateArg& arg);

bool SimpleRttiTemplateArgs(const NamedTypeInfo* entity)
{
	if (!entity->spec_template)
		return true;
	for (size_t i = 0; i < entity->spec_args.size(); i++)
		if (!SimpleRttiTemplateArg(entity->spec_args[i]))
			return false;
	return true;
}

bool SimpleRttiTemplateArg(const TemplateArg& arg)
{
	for (size_t i = 0; i < arg.pack_elements.size(); i++)
		if (!SimpleRttiTemplateArg(arg.pack_elements[i]))
			return false;
	if (arg.is_value || arg.template_entity)
		return true;
	if (!arg.type)
		return !arg.pack_elements.empty();
	TypePtr bare = RemoveTopCv(arg.type);
	if (bare->kind == TK_FUNDAMENTAL || bare->kind == TK_ENUM)
		return true;
	if (bare->kind == TK_CLASS)
		return SimpleRttiTemplateArgs(bare->named);
	return false;
}

}  // namespace

LowVTableInfo& LowerProgram::VTableEntry(const ClassInfo* cls)
{
	map<const ClassInfo*, size_t>::iterator found =
		vtable_index_.find(cls);
	if (found != vtable_index_.end())
		return vtables_[found->second];
	LowVTableInfo info;
	info.cls = cls;
	if (SimpleRttiTemplateArgs(cls->entity))
		info.low_name = UniqueSymbol(
			LowerScopePath(cls->members->parent) +
			LowerSanitizeName(cls->members->name) + "__vtable");
	else
		info.low_name = UniqueSymbol(
			"__vtable_type_" + MangleClassTypeEncoding(cls->entity));
	info.object_name = "_ZTV" + MangleClassTypeEncoding(cls->entity);
	vtable_index_[cls] = vtables_.size();
	vtables_.push_back(info);
	return vtables_.back();
}

string LowerProgram::VTableRef(const ClassInfo* cls)
{
	LowVTableInfo& entry = VTableEntry(cls);
	entry.used = true;
	return "@" + entry.low_name;
}

string LowerProgram::ExternalRttiVtableRef(ERttiVtableKind kind)
{
	static const char* const tails[5] = {
		"__class_type_info", "__si_class_type_info",
		"__fundamental_type_info", "__pointer_type_info",
		"__vmi_class_type_info"};
	static const char* const objects[5] = {
		"_ZTVN10__cxxabiv117__class_type_infoE",
		"_ZTVN10__cxxabiv120__si_class_type_infoE",
		"_ZTVN10__cxxabiv123__fundamental_type_infoE",
		"_ZTVN10__cxxabiv119__pointer_type_infoE",
		"_ZTVN10__cxxabiv121__vmi_class_type_infoE"};
	string& name = external_rtti_vtable_names_[kind];
	if (name.empty())
	{
		name = UniqueSymbol(string("__external_rtti_vtable__") +
		                    tails[kind]);
		poly_declare_globals_.push_back(
			"declare global @" + name + " [binding=strong, object=" +
			objects[kind] + "]");
	}
	return "@" + name;
}

string LowerProgram::ExternalRuntimeFnRef(const string& object_name,
                                          const string& declare_suffix)
{
	map<string, string>::iterator found =
		runtime_fn_names_.find(object_name);
	if (found != runtime_fn_names_.end())
		return "@" + found->second;
	string low = UniqueSymbol("__external_runtime__" + object_name);
	runtime_fn_names_[object_name] = low;
	runtime_declares_.push_back("declare function @" + low +
	                            declare_suffix);
	return "@" + low;
}

namespace {

// PA27: RTTI record names omit the unnamed-namespace placeholder (the
// reference presentation; the object names keep the full mangling).
string RttiScopePath(const Scope* scope)
{
	vector<string> parts;
	for (; scope && scope->parent; scope = scope->parent)
	{
		if (scope->kind != SCOPE_NAMESPACE && scope->kind != SCOPE_CLASS)
			continue;
		if (scope->name.empty())
			continue;
		parts.insert(parts.begin(), scope->name);
	}
	string path;
	for (size_t i = 0; i < parts.size(); i++)
		path += LowerSanitizeName(parts[i]) + "__";
	return path;
}

}  // namespace

// The RTTI record of `cls`, rendered on first use together with its
// typeinfo-name byte data; a derived class's record links the direct
// base's, so the whole chain renders transitively.
string LowerProgram::RttiRef(const ClassInfo* cls)
{
	map<const ClassInfo*, string>::iterator found =
		rtti_names_.find(cls);
	if (found != rtti_names_.end())
		return "@" + found->second;
	string encoding = MangleClassTypeEncoding(cls->entity);
	string low;
	string name_low;
	if (SimpleRttiTemplateArgs(cls->entity))
	{
		string tail = cls->entity->class_key + "_" +
			RttiScopePath(cls->members->parent) +
			LowerSanitizeName(cls->members->name);
		low = UniqueSymbol("__rtti_" + tail);
		name_low = UniqueSymbol("__typeinfo_name__" + tail);
	}
	else
	{
		low = UniqueSymbol("__rtti_type_" + encoding);
		name_low = UniqueSymbol("__typeinfo_name_type_" + encoding);
	}
	rtti_names_[cls] = low;
	string name_body;
	for (size_t i = 0; i < encoding.size(); i++)
		name_body += "  i8 " + to_string((int)encoding[i]) + "\n";
	name_body += "  i8 0\n";
	poly_globals_.push_back(
		"global @" + name_low +
		" [storage=readonly, binding=weak, object=_ZTS" + encoding +
		"] = {\n" + name_body + "}");
	// PA27 kind selection: no direct bases -> __class_type_info; one
	// public non-virtual base at offset 0 -> __si_class_type_info;
	// everything else (multiple bases, virtual bases, displaced or
	// non-public single bases) -> __vmi_class_type_info with one
	// offset-flags row per direct base in declaration order.
	bool si = cls->direct_bases.size() == 1 &&
		!cls->direct_bases[0].is_virtual &&
		cls->direct_bases[0].offset == 0 &&
		cls->direct_bases[0].access == MA_PUBLIC;
	if (cls->direct_bases.empty() && cls->base)
		si = true;  // link-only records keep the PA17 shape
	string items;
	if (cls->direct_bases.empty() || si)
	{
		items = "  ptr addr " +
			ExternalRttiVtableRef(cls->base ? RTTI_VT_SI_CLASS
			                                : RTTI_VT_CLASS) + " + 16\n" +
			"  ptr addr @" + name_low + "\n";
		if (cls->base)
			items += "  ptr addr " + RttiRef(cls->base) + "\n";
	}
	else
	{
		items = "  ptr addr " + ExternalRttiVtableRef(RTTI_VT_VMI_CLASS) +
			" + 16\n  ptr addr @" + name_low + "\n  i32 0\n  i32 " +
			to_string(cls->direct_bases.size()) + "\n";
		for (size_t b = 0; b < cls->direct_bases.size(); b++)
		{
			const ClassDirectBase& row = cls->direct_bases[b];
			long long offset_part;
			long long flags = row.access == MA_PUBLIC ? 2 : 0;
			if (row.is_virtual)
			{
				// The offset field names the vtable-header slot the
				// vbase offset lives in (address point - 24 - 8k).
				const ClassVBase* entry = FindClassVBase(*cls, row.cls);
				size_t index = entry ? (size_t)(entry - &cls->vbases[0])
				                     : 0;
				offset_part = -(long long)(24 + 8 * index);
				flags |= 1;
			}
			else
				offset_part = (long long)row.offset;
			items += "  ptr addr " + RttiRef(row.cls) + "\n  i64 " +
				to_string((offset_part << 8) | flags) + "\n";
		}
	}
	poly_globals_.push_back(
		"global @" + low +
		" [storage=readonly, binding=weak, object=_ZTI" + encoding +
		"] = {\n" + items + "}");
	return "@" + low;
}

// The RTTI record of an arbitrary typeid operand type. Complete
// classes route through their class records; fundamentals, pointers,
// and never-completed class specializations render encoding-keyed
// records on first use.
string LowerProgram::RttiTypeRef(const TypePtr& type_in)
{
	TypePtr type = RemoveTopCv(type_in);
	if (type->kind == TK_CLASS && type->named->class_record)
		return RttiRef(type->named->class_record);
	string encoding = MangleTypeEncoding(type);
	map<string, string>::iterator found =
		rtti_type_names_.find(encoding);
	if (found != rtti_type_names_.end())
		return "@" + found->second;
	string low;
	string name_low;
	string head;
	string extra;
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
	{
		string tail = FundamentalNameTail(type);
		low = UniqueSymbol("__rtti_" + tail);
		name_low = UniqueSymbol("__typeinfo_name__" + tail);
		head = ExternalRttiVtableRef(RTTI_VT_FUNDAMENTAL);
		break;
	}
	case TK_POINTER:
	{
		low = UniqueSymbol("__rtti_type_" + encoding);
		name_low = UniqueSymbol("__typeinfo_name_type_" + encoding);
		head = ExternalRttiVtableRef(RTTI_VT_POINTER);
		// __pointer_type_info: qualifier flags (0x8 marks an
		// incomplete pointee class), then the pointee's record.
		rtti_type_names_[encoding] = low;
		{
			TypePtr pointee = RemoveTopCv(type->target);
			bool incomplete = pointee->kind == TK_CLASS &&
				(!pointee->named->class_record ||
				 !pointee->named->complete);
			extra = string("  i32 ") + (incomplete ? "8" : "0") +
				"\n  ptr addr " + RttiTypeRef(type->target) + "\n";
		}
		break;
	}
	case TK_CLASS:
		// A never-completed specialization: a plain class record with
		// no base information.
		low = UniqueSymbol("__rtti_type_" + encoding);
		name_low = UniqueSymbol("__typeinfo_name_type_" + encoding);
		head = ExternalRttiVtableRef(RTTI_VT_CLASS);
		break;
	default:
		throw runtime_error("typeid operand type is outside the PA25 "
		                    "assignment boundary");
	}
	rtti_type_names_[encoding] = low;
	string name_body;
	for (size_t i = 0; i < encoding.size(); i++)
		name_body += "  i8 " + to_string((int)encoding[i]) + "\n";
	name_body += "  i8 0\n";
	poly_globals_.push_back(
		"global @" + name_low +
		" [storage=readonly, binding=weak, object=_ZTS" + encoding +
		"] = {\n" + name_body + "}");
	poly_globals_.push_back(
		"global @" + low +
		" [storage=readonly, binding=weak, object=_ZTI" + encoding +
		"] = {\n  ptr addr " + head + " + 16\n  ptr addr @" + name_low +
		"\n" + extra + "}");
	return "@" + low;
}

// The @__cxa_pure_virtual stand-in for pure slots, declared with the
// lowered signature of the first pure virtual that demands it.
void LowerProgram::EnsurePureVirtualDeclare(const TypePtr& adjusted)
{
	if (!pure_virtual_name_.empty())
		return;
	pure_virtual_name_ = UniqueSymbol("__cxa_pure_virtual");
	string ret_text;
	bool indirect_result = LowerAbiReturn(adjusted->target, ret_text);
	string params;
	size_t at = 0;
	if (indirect_result)
	{
		params = "%arg0 : ptr [pass=indirect_result]";
		at = 1;
	}
	for (size_t i = 0; i < adjusted->parameters.size(); i++, at++)
	{
		string param_text;
		string pass;
		LowerAbiParameter(adjusted->parameters[i], param_text, pass);
		params += (at ? ", " : "") + string("%arg") + to_string(at) +
			" : " + param_text;
		if (!pass.empty())
			params += " [pass=" + pass + "]";
	}
	pure_virtual_declare_ = "declare function @" + pure_virtual_name_ +
		"(" + params + ") -> " + ret_text +
		" [effects=readnone, unwind=no, return=noreturn, "
		"binding=strong]";
}

// PA27: the address point of a class's primary vtable (past the
// vbase-offset entries, the offset-to-top, and the RTTI pointer) and
// of its views' vtables (headerless when the class has no virtual
// bases).
unsigned long long LowerProgram::VTableAddressPoint(const ClassInfo* cls)
{
	return 8 * (2 + (ClassHasVBases(*cls) ? cls->vbases.size() : 0));
}

unsigned long long LowerProgram::ViewAddressPoint(const ClassInfo* cls)
{
	return ClassHasVBases(*cls) ? 8 * (2 + cls->vbases.size()) : 0;
}

// PA29: a view whose header follows its own class's vbase table has
// its address point after that (possibly shorter) header.
unsigned long long LowerProgram::ViewGroupAddressPoint(
	const ClassInfo& cls, const ClassView& view)
{
	if (!ClassHasVBases(cls))
		return 0;
	const ClassInfo* header_of =
		cls.vbase_views_use_own_tables && view.cls ? view.cls : &cls;
	return 8 * (2 + (ClassHasVBases(*header_of)
	                     ? header_of->vbases.size() : 0));
}

void LowerProgram::VTableGroupRef(const ClassInfo* cls)
{
	LowVTableInfo& entry = VTableEntry(cls);
	entry.used = true;
	if (!cls->views.empty() ||
	    (cls->is_polymorphic && ClassHasVBases(*cls)))
		entry.group_used = true;
}

string LowerProgram::ViewVTableRef(const ClassInfo* cls,
                                   size_t view_index)
{
	VTableGroupRef(cls);
	std::pair<const ClassInfo*, size_t> key(cls, view_index);
	map<std::pair<const ClassInfo*, size_t>, string>::iterator found =
		view_names_.find(key);
	if (found != view_names_.end())
		return "@" + found->second;
	const ClassView& view = cls->views[view_index];
	string derived_tail = LowerScopePath(cls->members->parent) +
		LowerSanitizeName(cls->members->name);
	string base_tail =
		LowerScopePath(view.cls->members->parent) +
		LowerSanitizeName(view.cls->members->name);
	string name = UniqueSymbol(derived_tail + "____view__" + base_tail +
	                           "__" + to_string(view.offset) +
	                           "__vtable");
	view_names_[key] = name;
	return "@" + name;
}

string LowerProgram::VttRef(const ClassInfo* cls)
{
	LowVTableInfo& entry = VTableEntry(cls);
	entry.used = true;
	entry.group_used = true;
	if (entry.vtt_low_name.empty())
		entry.vtt_low_name = UniqueSymbol(
			LowerScopePath(cls->members->parent) +
			LowerSanitizeName(cls->members->name) + "____vtt");
	return "@" + entry.vtt_low_name;
}

namespace {

// Whether a direct non-virtual base contributes a construction
// sub-block to its derived class's VTT.
bool VttQualifies(const ClassInfo& base)
{
	return base.is_polymorphic && ClassHasVBases(base);
}

// The outermost views of a class (the vpointer locations its
// constructor entries set beside the primary).
void OutermostViews(const ClassInfo& cls, vector<size_t>& out)
{
	out.clear();
	for (size_t i = 0; i < cls.views.size(); i++)
		if (cls.views[i].outermost)
			out.push_back(i);
}

}  // namespace

size_t LowerProgram::VttShapeSize(const ClassInfo& cls)
{
	size_t size = 1;
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
		if (!cls.direct_bases[b].is_virtual &&
		    VttQualifies(*cls.direct_bases[b].cls))
			size += VttShapeSize(*cls.direct_bases[b].cls);
	// PA29: qualifying virtual direct bases carry construction
	// sub-blocks after the non-virtual ones (their base entries also
	// take a construction context from the complete object's ctor).
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
		if (cls.direct_bases[b].is_virtual &&
		    VttQualifies(*cls.direct_bases[b].cls))
			size += VttShapeSize(*cls.direct_bases[b].cls);
	vector<size_t> views;
	OutermostViews(cls, views);
	return size + views.size();
}

size_t LowerProgram::SubVttIndex(const ClassInfo& derived,
                                 const ClassInfo& base)
{
	size_t at = 1;
	for (size_t b = 0; b < derived.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = derived.direct_bases[b];
		if (row.is_virtual || !VttQualifies(*row.cls))
			continue;
		if (row.cls == &base)
			return at;
		at += VttShapeSize(*row.cls);
	}
	for (size_t b = 0; b < derived.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = derived.direct_bases[b];
		if (!row.is_virtual || !VttQualifies(*row.cls))
			continue;
		if (row.cls == &base)
			return at;
		at += VttShapeSize(*row.cls);
	}
	throw runtime_error("construction table sub-block missing");
}

size_t LowerProgram::OwnViewVttIndex(const ClassInfo& cls, size_t ordinal)
{
	vector<size_t> views;
	OutermostViews(cls, views);
	return VttShapeSize(cls) - views.size() + ordinal;
}

// A this-adjusting vtable entry thunk toward `target`. The LowIR name
// derives from the target's; the object name is the Itanium _ZThn
// spelling (dropped for internal targets).
// PA29 5.5: a pointer to a virtual member function dispatches on the
// dynamic type, so the pair carries an internal forwarding thunk (the
// reference's <fn>__member_pointer_thunk shape) instead of the static
// code address. Returns "" for non-virtual members.
string LowerProgram::MemberPointerThunkRef(const SemNode& member)
{
	const NamedTypeInfo* owner = member.entity_scope
		? member.entity_scope->entity : 0;
	const ClassInfo* record = owner ? owner->class_record : 0;
	if (!record || !record->is_polymorphic)
		return "";
	int slot = FindVirtualSlotIndex(*record, member.entity_name,
	                                member.type);
	// The member reference usually carries the this-adjusted type;
	// match the slot's adjusted signature directly then.
	for (size_t i = 0; slot < 0 && i < record->vslots.size(); i++)
	{
		const VirtualSlot& vs = record->vslots[i];
		if (vs.kind != VS_METHOD || vs.name != member.entity_name ||
		    !vs.type || !member.type ||
		    vs.type->parameters.size() != member.type->parameters.size())
			continue;
		bool same = true;
		for (size_t p = 1; p < vs.type->parameters.size(); p++)
			if (!TypeEquals(vs.type->parameters[p],
			                member.type->parameters[p]))
				same = false;
		if (same)
			slot = (int)i;
	}
	if (slot < 0)
		return "";
	std::pair<const ClassInfo*, size_t> key(record, (size_t)slot);
	map<std::pair<const ClassInfo*, size_t>, string>::iterator found =
		pm_thunk_names_.find(key);
	if (found != pm_thunk_names_.end())
		return "@" + found->second;
	LowFunctionInfo& target = CalleeEntryInfo(member);
	string name =
		UniqueSymbol(target.low_name + "__member_pointer_thunk");
	pm_thunk_names_[key] = name;

	const TypePtr& fn_type = member.type;
	string ret = fn_type->target && !IsVoidType(fn_type->target)
		? LowerValueType(fn_type->target) : "void";
	string params = "%this : ptr";
	string signature = "%arg0 : ptr";
	string arguments = "%this";
	for (size_t i = 0; i < fn_type->parameters.size(); i++)
	{
		string spelled = LowerValueType(fn_type->parameters[i]);
		string arg = "%p" + to_string(i);
		params += ", " + arg + " : " + spelled;
		signature += ", %arg" + to_string(i + 1) + " : " + spelled;
		arguments += ", " + arg;
	}
	string body;
	body += "  block ^entry:\n";
	body += "    %t1 = load ptr %this\n";
	string slot_address = "%t1";
	int temp = 2;
	if (slot != 0)
	{
		body += "    %t2 = index i8 %t1, " + to_string(8 * slot) +
			"\n";
		slot_address = "%t2";
		temp = 3;
	}
	string fn = "%t" + to_string(temp++);
	body += "    " + fn + " = load ptr " + slot_address + "\n";
	if (ret == "void")
	{
		body += "    call void " + fn + "(" + arguments + ") as (" +
			signature + ") -> void\n    return void\n";
	}
	else
	{
		string result = "%t" + to_string(temp);
		body += "    " + result + " = call " + ret + " " + fn + "(" +
			arguments + ") as (" + signature + ") -> " + ret +
			"\n    return " + ret + " " + result + "\n";
	}
	thunk_texts_.push_back(
		"function @" + name + "(" + params + ") -> " + ret +
		" [binding=internal] {\n" + body + "}");
	return "@" + name;
}

string LowerProgram::VTableThunkRef(LowFunctionInfo& target,
                                    long long adjust,
                                    const TypePtr& adjusted)
{
	std::pair<string, long long> key(target.low_name, adjust);
	map<std::pair<string, long long>, string>::iterator found =
		thunk_names_.find(key);
	if (found != thunk_names_.end())
		return "@" + found->second;
	string name = UniqueSymbol("_" + target.low_name +
	                           "__vtable_return_adjust");
	thunk_names_[key] = name;
	DemandFunction(target);
	string ret_text;
	bool indirect_result = LowerAbiReturn(adjusted->target, ret_text);
	vector<string> arg_names;
	string params;
	size_t at = 0;
	if (indirect_result)
	{
		params = "%ret : ptr [pass=indirect_result]";
		arg_names.push_back("%ret");
		at = 1;
	}
	for (size_t i = 0; i < adjusted->parameters.size(); i++, at++)
	{
		string param_text;
		string pass;
		LowerAbiParameter(adjusted->parameters[i], param_text, pass);
		string arg = "%arg" + to_string(at);
		params += (at ? ", " : "") + arg + " : " + param_text;
		if (!pass.empty())
			params += " [pass=" + pass + "]";
		arg_names.push_back(arg);
	}
	string meta = target.internal
		? " [binding=internal]"
		: " [binding=weak" +
			(target.object_name.empty()
				? string()
				: ", object=_ZThn" +
					to_string(adjust < 0 ? -adjust : adjust) + "_" +
					target.object_name.substr(2)) + "]";
	string body = "function @" + name + "(" + params + ") -> " +
		ret_text + meta + " {\n  block ^entry:\n";
	size_t this_at = indirect_result ? 1 : 0;
	body += "    %t1 = index i8 " + arg_names[this_at] + ", " +
		to_string(adjust) + "\n";
	arg_names[this_at] = "%t1";
	string call_args;
	for (size_t i = 0; i < arg_names.size(); i++)
		call_args += (i ? ", " : "") + arg_names[i];
	if (ret_text == "void")
		body += "    call void @" + target.low_name + "(" + call_args +
			")\n    return void\n}";
	else
		body += "    %t2 = call " + ret_text + " @" + target.low_name +
			"(" + call_args + ")\n    return " + ret_text + " %t2\n}";
	thunk_texts_.push_back(body);
	return "@" + name;
}

// One slot list rendered into vtable items: `paired` slot lists carry
// [entry, i64 this-adjust] pairs; unpaired lists route non-zero
// adjustments through entry thunks.
string LowerProgram::RenderVTableSlots(const ClassInfo& derived,
                                       const ClassView* view,
                                       const ClassInfo* level,
                                       unsigned long long level_offset,
                                       unsigned long long position)
{
	string body;
	const ClassInfo& slots_of = view ? *view->cls : derived;
	bool paired = ClassHasVBases(slots_of);
	for (size_t i = 0; i < slots_of.vslots.size(); i++)
	{
		const VirtualSlot& slot = slots_of.vslots[i];
		string target;
		long long adjust = 0;
		bool pure = slot.kind == VS_METHOD && slot.pure;
		const Scope* owner = slot.owner;
		TypePtr type = slot.type;
		if (view)
		{
			ViewOverrider over;
			if (!ResolveViewOverrider(level ? *level : derived, *view,
			                          slot, over))
				throw runtime_error("view overrider unresolved");
			owner = over.owner;
			type = over.type;
			pure = slot.kind == VS_METHOD && over.pure;
			unsigned long long overrider_at = over.at_view_base
				? position : level_offset + over.offset;
			adjust = (long long)overrider_at - (long long)position;
		}
		if (pure)
		{
			EnsurePureVirtualDeclare(type);
			target = "@" + pure_virtual_name_;
		}
		else
		{
			const char* code = slot.kind == VS_METHOD ? ""
				: slot.kind == VS_DTOR_COMPLETE ? "D1" : "D0";
			string name = slot.kind == VS_METHOD
				? slot.name
				: "~" + owner->name;
			LowFunctionInfo& fn =
				MemberFunctionEntry(owner, name, type, code);
			DemandFunction(fn);
			if (!paired && adjust != 0)
				target = VTableThunkRef(fn, adjust, type);
			else
				target = "@" + fn.low_name;
		}
		body += "  ptr addr " + target + "\n";
		if (paired)
			body += "  i64 " + to_string(adjust) + "\n";
	}
	return body;
}

// The header items of one vtable: the (reversed) vbase offsets of
// `table_of`'s virtual bases located inside the complete `outer`
// object, the offset-to-top, and the RTTI pointer.
string LowerProgram::RenderVTableHeader(const ClassInfo& outer,
                                        const ClassInfo& table_of,
                                        unsigned long long position,
                                        const string& rtti)
{
	string body;
	if (ClassHasVBases(table_of))
		for (size_t k = table_of.vbases.size(); k-- > 0;)
		{
			const ClassVBase* entry =
				FindClassVBase(outer, table_of.vbases[k].cls);
			long long at = entry ? (long long)entry->offset
			                     : (long long)table_of.vbases[k].offset;
			body += "  i64 " +
				to_string(at - (long long)position) + "\n";
		}
	body += "  i64 " + to_string(-(long long)position) + "\n  ptr addr " +
		rtti + "\n";
	return body;
}

string LowerProgram::RenderVTableDefinition(LowVTableInfo& entry)
{
	const ClassInfo& cls = *entry.cls;
	string body;
	if (ClassHasVBases(cls))
		body = RenderVTableHeader(cls, cls, 0, RttiRef(&cls));
	else
		body = "  i64 0\n  ptr addr " + RttiRef(&cls) + "\n";
	body += RenderVTableSlots(cls, 0, 0, 0, 0);
	string text = "global @" + entry.low_name +
		" [storage=readonly, binding=" +
		(entry.strong ? "strong" : "weak") + ", object=" +
		entry.object_name + "] = {\n" + body + "}";
	if (!entry.group_used)
		return text;
	// PA27: the group - every secondary view, then (for polymorphic
	// virtual-base classes) the construction vtables and the VTT.
	for (size_t v = 0; v < cls.views.size(); v++)
	{
		const ClassView& view = cls.views[v];
		string name = ViewVTableRef(&cls, v);
		string view_body;
		// A view's header rows are indexed by the view class's own
		// static vbase table (`vptr - 24 - 8k`): when some subobject's
		// table order disagrees with the complete class's, the view
		// carries rows in its own class's order (PA29). Agreeing
		// hierarchies keep the complete-class rows (the PA27 shapes).
		const ClassInfo& header_of =
			cls.vbase_views_use_own_tables && view.cls ? *view.cls : cls;
		if (ClassHasVBases(header_of))
			view_body = RenderVTableHeader(cls, header_of, view.offset,
			                               RttiRef(&cls));
		view_body += RenderVTableSlots(cls, &view, &cls, 0, view.offset);
		text += "\nglobal " + name +
			" [storage=readonly, binding=weak, object=" + name +
			"] = {\n" + view_body + "}";
	}
	if (cls.is_polymorphic && ClassHasVBases(cls))
		text += RenderConstructionGroup(entry);
	return text;
}

// The construction vtables of every qualifying base subobject and the
// VTT global. Construction vtable K of base X covers X's own entries
// (s0 = X's primary shape, s1.. = X's outermost views), positioned in
// the complete object's layout.
string LowerProgram::RenderConstructionGroup(LowVTableInfo& entry)
{
	const ClassInfo& cls = *entry.cls;
	string text;
	vector<std::pair<const ClassInfo*, unsigned long long>> order;
	CollectConstructionBases(cls, 0, order);
	map<std::pair<const ClassInfo*, unsigned long long>,
	    vector<string>> names;
	string derived_tail = LowerScopePath(cls.members->parent) +
		LowerSanitizeName(cls.members->name);
	for (size_t i = 0; i < order.size(); i++)
	{
		const ClassInfo& base = *order[i].first;
		unsigned long long offset = order[i].second;
		string base_tail = LowerScopePath(base.members->parent) +
			LowerSanitizeName(base.members->name);
		vector<string>& group = names[order[i]];
		// s0: the base's primary shape at its position.
		string name = UniqueSymbol(
			derived_tail + "____construction__" + base_tail + "__" +
			to_string(offset) + "__s0__vtable");
		group.push_back(name);
		string body = RenderVTableHeader(cls, base, offset,
		                                 RttiRef(&base));
		body += RenderVTableSlots(base, 0, 0, 0, 0);
		text += "\nglobal @" + name +
			" [storage=readonly, binding=weak, object=@" + name +
			"] = {\n" + body + "}";
		// s1..: the base's outermost views - headers positioned in the
		// complete object, slots as in the base's own view (thunks and
		// adjustments follow the base's own layout).
		vector<size_t> views;
		OutermostViews(base, views);
		for (size_t v = 0; v < views.size(); v++)
		{
			const ClassView& view = base.views[views[v]];
			unsigned long long position = view.is_virtual
				? VBasePositionIn(cls, view.cls)
				: offset + view.offset;
			string view_name = UniqueSymbol(
				derived_tail + "____construction__" + base_tail + "__" +
				to_string(offset) + "__s" + to_string(v + 1) +
				"__vtable");
			group.push_back(view_name);
			string view_body = RenderVTableHeader(cls, base, position,
			                                      RttiRef(view.cls));
			view_body += RenderVTableSlots(base, &view, &base, 0,
			                               view.offset);
			text += "\nglobal @" + view_name +
				" [storage=readonly, binding=weak, object=@" +
				view_name + "] = {\n" + view_body + "}";
		}
	}
	// The VTT: the primary address point, the construction sub-blocks
	// in shape order, then the class's own view address points.
	VttRef(&cls);
	string items = "  ptr addr @" + entry.low_name + " + " +
		to_string(VTableAddressPoint(&cls)) + "\n";
	AppendVttEntries(cls, 0, names, items);
	vector<size_t> views;
	OutermostViews(cls, views);
	for (size_t v = 0; v < views.size(); v++)
		items += "  ptr addr " + ViewVTableRef(&cls, views[v]) + " + " +
			to_string(ViewGroupAddressPoint(cls, cls.views[views[v]])) +
			"\n";
	text += "\nglobal @" + entry.vtt_low_name +
		" [storage=readonly, binding=weak, object=_ZTT" +
		MangleClassTypeEncoding(cls.entity) +
		", object_root=yes] = {\n" + items + "}";
	return text;
}

// The construction bases of the complete class, pre-order (a base's
// own entries before its nested qualifying bases).
void LowerProgram::CollectConstructionBases(
	const ClassInfo& cls, unsigned long long offset,
	vector<std::pair<const ClassInfo*, unsigned long long>>& out)
{
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = cls.direct_bases[b];
		if (row.is_virtual || !VttQualifies(*row.cls))
			continue;
		out.push_back(std::make_pair(row.cls, offset + row.offset));
		CollectConstructionBases(*row.cls, offset + row.offset, out);
	}
	// PA29: qualifying virtual direct bases sit at their shared
	// virtual-base position of the enclosing subobject's layout.
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = cls.direct_bases[b];
		if (!row.is_virtual || !VttQualifies(*row.cls))
			continue;
		unsigned long long at = offset + VBasePositionIn(cls, row.cls);
		out.push_back(std::make_pair(row.cls, at));
		CollectConstructionBases(*row.cls, at, out);
	}
}

// VTT entries of one class's sub-blocks in shape order: each base's
// s0, its own nested blocks recursively, then its s1.. views.
void LowerProgram::AppendVttEntries(
	const ClassInfo& cls, unsigned long long offset,
	const map<std::pair<const ClassInfo*, unsigned long long>,
	          vector<string>>& names,
	string& items)
{
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = cls.direct_bases[b];
		if (row.is_virtual || !VttQualifies(*row.cls))
			continue;
		unsigned long long at = offset + row.offset;
		map<std::pair<const ClassInfo*, unsigned long long>,
		    vector<string>>::const_iterator found =
			names.find(std::make_pair(row.cls, at));
		if (found == names.end())
			throw runtime_error("construction table block missing");
		const vector<string>& group = found->second;
		unsigned long long point = VTableAddressPoint(row.cls);
		items += "  ptr addr @" + group[0] + " + " + to_string(point) +
			"\n";
		AppendVttEntries(*row.cls, at, names, items);
		for (size_t v = 1; v < group.size(); v++)
			items += "  ptr addr @" + group[v] + " + " +
				to_string(point) + "\n";
	}
	// PA29: the qualifying virtual direct bases' sub-blocks follow the
	// non-virtual ones (matching SubVttIndex).
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = cls.direct_bases[b];
		if (!row.is_virtual || !VttQualifies(*row.cls))
			continue;
		unsigned long long at = offset + VBasePositionIn(cls, row.cls);
		map<std::pair<const ClassInfo*, unsigned long long>,
		    vector<string>>::const_iterator found =
			names.find(std::make_pair(row.cls, at));
		if (found == names.end())
			throw runtime_error("construction table block missing");
		const vector<string>& group = found->second;
		unsigned long long point = VTableAddressPoint(row.cls);
		items += "  ptr addr @" + group[0] + " + " + to_string(point) +
			"\n";
		AppendVttEntries(*row.cls, at, names, items);
		for (size_t v = 1; v < group.size(); v++)
			items += "  ptr addr @" + group[v] + " + " +
				to_string(point) + "\n";
	}
}

// The complete-object position of a virtual base.
unsigned long long LowerProgram::VBasePositionIn(const ClassInfo& outer,
                                                 const ClassInfo* base)
{
	const ClassVBase* entry = FindClassVBase(outer, base);
	if (!entry)
		throw runtime_error("virtual base position missing");
	return entry->offset;
}

bool LowerProgram::RenderPendingVTables()
{
	bool any = false;
	for (size_t i = 0; i < vtables_.size(); i++)
	{
		LowVTableInfo& entry = vtables_[i];
		if (!entry.used ||
		    (entry.rendered && entry.group_rendered == entry.group_used))
			continue;
		entry.rendered = true;
		entry.group_rendered = entry.group_used;
		any = true;
		// A class whose key function is defined in another translation
		// unit gets its vtable there; this program only declares it.
		if (!entry.cls->key_name.empty() && !entry.strong)
			poly_declare_globals_.push_back(
				"declare global @" + entry.low_name +
				" [binding=strong, object=" + entry.object_name + "]");
		else
			entry.text = RenderVTableDefinition(entry);
	}
	return any;
}

// PA25 15.1/15.3: the record a thrown or caught type matches by. The
// runtime compares record addresses, so fundamentals resolve to the
// C++ library's strong records (with the weak scaffolding rendered
// beside them); program classes match their own weak records.
string LowerProgram::ThrowRttiRef(const TypePtr& type_in)
{
	TypePtr type = RemoveTopCv(type_in);
	if (type->kind != TK_FUNDAMENTAL)
		return RttiTypeRef(type);
	RttiTypeRef(type);
	string encoding = MangleTypeEncoding(type);
	map<string, string>::iterator found =
		external_rtti_names_.find(encoding);
	if (found != external_rtti_names_.end())
		return "@" + found->second;
	string name = UniqueSymbol("__external_rtti__" +
	                           FundamentalNameTail(type));
	external_rtti_names_[encoding] = name;
	poly_declare_globals_.push_back(
		"declare global @" + name + " [binding=strong, object=_ZTI" +
		encoding + "]");
	return "@" + name;
}

string LowerProgram::ExternalVoidRttiRef()
{
	map<string, string>::iterator found = external_rtti_names_.find("v");
	if (found != external_rtti_names_.end())
		return "@" + found->second;
	string name = UniqueSymbol("__external_rtti__void");
	external_rtti_names_["v"] = name;
	poly_declare_globals_.push_back(
		"declare global @" + name + " [binding=strong, object=_ZTIv]");
	return "@" + name;
}

// The weak zero-filled exception-object scaffolding global beside a
// throw of `type` (the reference presentation).
void LowerProgram::EhObjGlobal(const TypePtr& type_in)
{
	TypePtr type = RemoveTopCv(type_in);
	string tail;
	if (type->kind == TK_CLASS)
		tail = type->named->class_key + "_" +
			LowerScopePath(type->named->class_record
			                   ? type->named->class_record->members->parent
			                   : 0) +
			LowerSanitizeName(type->named->name);
	else
		tail = FundamentalNameTail(type);
	if (!ehobj_rendered_.insert(tail).second)
		return;
	string name = UniqueSymbol("__ehobj_" + tail);
	poly_globals_.push_back(
		"global @" + name + " [binding=weak, object=@" + name +
		"] = {\n  zero " + to_string(TypeSize(type)) + "\n}");
}
