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

LowVTableInfo& LowerProgram::VTableEntry(const ClassInfo* cls)
{
	map<const ClassInfo*, size_t>::iterator found =
		vtable_index_.find(cls);
	if (found != vtable_index_.end())
		return vtables_[found->second];
	LowVTableInfo info;
	info.cls = cls;
	info.low_name = UniqueSymbol(
		LowerScopePath(cls->members->parent) +
		LowerSanitizeName(cls->members->name) + "__vtable");
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

string LowerProgram::ExternalRttiVtableRef(bool si)
{
	string& name = si ? external_si_rtti_name_
	                  : external_class_rtti_name_;
	if (name.empty())
	{
		name = UniqueSymbol(
			si ? "__external_rtti_vtable____si_class_type_info"
			   : "__external_rtti_vtable____class_type_info");
		poly_declare_globals_.push_back(
			"declare global @" + name + " [binding=strong, object=" +
			(si ? "_ZTVN10__cxxabiv120__si_class_type_infoE"
			    : "_ZTVN10__cxxabiv117__class_type_infoE") + "]");
	}
	return "@" + name;
}

// The RTTI record of `cls`, rendered on first use together with its
// typeinfo-name byte data; a derived class's record links the direct
// base's, so the whole chain renders transitively.
string LowerProgram::RttiRef(const ClassInfo* cls)
{
	map<const ClassInfo*, string>::iterator found =
		rtti_names_.find(cls);
	if (found != rtti_names_.end())
		return "@" + found->second;
	string tail = cls->entity->class_key + "_" +
		LowerScopePath(cls->members->parent) +
		LowerSanitizeName(cls->members->name);
	string encoding = MangleClassTypeEncoding(cls->entity);
	string low = UniqueSymbol("__rtti_" + tail);
	rtti_names_[cls] = low;
	string name_low = UniqueSymbol("__typeinfo_name__" + tail);
	string name_body;
	for (size_t i = 0; i < encoding.size(); i++)
		name_body += "  i8 " + to_string((int)encoding[i]) + "\n";
	name_body += "  i8 0\n";
	poly_globals_.push_back(
		"global @" + name_low +
		" [storage=readonly, binding=weak, object=_ZTS" + encoding +
		"] = {\n" + name_body + "}");
	string items = "  ptr addr " +
		ExternalRttiVtableRef(cls->base != 0) + " + 16\n" +
		"  ptr addr @" + name_low + "\n";
	if (cls->base)
		items += "  ptr addr " + RttiRef(cls->base) + "\n";
	poly_globals_.push_back(
		"global @" + low +
		" [storage=readonly, binding=weak, object=_ZTI" + encoding +
		"] = {\n" + items + "}");
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

string LowerProgram::RenderVTableDefinition(LowVTableInfo& entry)
{
	const ClassInfo& cls = *entry.cls;
	string body = "  i64 0\n  ptr addr " + RttiRef(&cls) + "\n";
	for (size_t i = 0; i < cls.vslots.size(); i++)
	{
		const VirtualSlot& slot = cls.vslots[i];
		string target;
		if (slot.kind == VS_METHOD && slot.pure)
		{
			EnsurePureVirtualDeclare(slot.type);
			target = "@" + pure_virtual_name_;
		}
		else
		{
			const char* code = slot.kind == VS_METHOD ? ""
				: slot.kind == VS_DTOR_COMPLETE ? "D1" : "D0";
			LowFunctionInfo& fn = MemberFunctionEntry(
				slot.owner, slot.name, slot.type, code);
			DemandFunction(fn);
			target = "@" + fn.low_name;
		}
		body += "  ptr addr " + target + "\n";
	}
	return "global @" + entry.low_name + " [storage=readonly, binding=" +
		(entry.strong ? "strong" : "weak") + ", object=" +
		entry.object_name + "] = {\n" + body + "}";
}

bool LowerProgram::RenderPendingVTables()
{
	bool any = false;
	for (size_t i = 0; i < vtables_.size(); i++)
	{
		LowVTableInfo& entry = vtables_[i];
		if (!entry.used || entry.rendered)
			continue;
		entry.rendered = true;
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
