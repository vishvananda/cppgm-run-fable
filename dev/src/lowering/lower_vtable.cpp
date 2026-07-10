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
	static const char* const tails[4] = {
		"__class_type_info", "__si_class_type_info",
		"__fundamental_type_info", "__pointer_type_info"};
	static const char* const objects[4] = {
		"_ZTVN10__cxxabiv117__class_type_infoE",
		"_ZTVN10__cxxabiv120__si_class_type_infoE",
		"_ZTVN10__cxxabiv123__fundamental_type_infoE",
		"_ZTVN10__cxxabiv119__pointer_type_infoE"};
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
			LowerScopePath(cls->members->parent) +
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
	string items = "  ptr addr " +
		ExternalRttiVtableRef(cls->base ? RTTI_VT_SI_CLASS
		                                : RTTI_VT_CLASS) + " + 16\n" +
		"  ptr addr @" + name_low + "\n";
	if (cls->base)
		items += "  ptr addr " + RttiRef(cls->base) + "\n";
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
