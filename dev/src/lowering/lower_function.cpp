#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

// Statement lowering and the shared block/slot/temp state. The
// expression half lives in lower_expr.cpp.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

TypePtr StripReference(const TypePtr& type)
{
	return IsReferenceType(type) ? type->target : type;
}

}  // namespace

FunctionLowerer::FunctionLowerer(LowerProgram& program,
                                 const SemNode& definition,
                                 const LowFunctionInfo& info)
	: program_(program), def_(definition), info_(info),
	  return_type_(definition.type->target),
	  indirect_ret_(false), nrvo_scope_(0), nrvo_active_(false),
	  temp_counter_(0), label_counter_(0), mat_counter_(0),
	  eh_open_(false), in_lifetime_action_(false), eh_armed_(false),
	  in_cleanup_emission_(false), ctor_depth_(0)
{
}

string FunctionLowerer::Lower()
{
	string ret_text;
	indirect_ret_ = LowerAbiReturn(return_type_, ret_text);
	if (indirect_ret_)
	{
		ParamInfo ret_param;
		ret_param.low_name = "ret";
		ret_param.type_text = "ptr";
		ret_param.pass = "indirect_result";
		params_.push_back(ret_param);
		param_names_.insert(ret_param.low_name);
		ScanReturnSlotReuse();
	}
	size_t first_statement = 0;
	for (size_t i = 0; i < def_.children.size(); i++)
	{
		const SemNode& child = *def_.children[i];
		if (child.kind != SN_PARAMETER)
			break;
		first_statement = i + 1;
		ParamInfo param;
		param.low_name = child.name.empty()
			? "__param" + to_string(i) : child.name;
		LowerAbiParameter(child.type, param.type_text, param.pass);
		// The named slot keeps the object's storage spelling even when
		// the value arrives by address.
		if (param.pass == "by_address")
			param.type_text = "ptr";
		params_.push_back(param);
		param_names_.insert(param.low_name);
	}
	EmitParameterStores();
	LowerStatementList(def_.children, first_statement);
	TerminateOpenEnd();
	return Render();
}

string FunctionLowerer::Header() const
{
	string params;
	for (size_t i = 0; i < params_.size(); i++)
	{
		if (i)
			params += ", ";
		params += "%" + params_[i].low_name + " : " +
			params_[i].type_text;
		if (!params_[i].pass.empty())
			params += " [pass=" + params_[i].pass + "]";
	}
	vector<string> meta;
	if (info_.is_main)
		meta.push_back("role=entry");
	if (!info_.role.empty())
		meta.push_back("role=" + info_.role);
	if (def_.type->variadic)
		meta.push_back("arity=variadic");
	if (info_.unwind_no)
		meta.push_back("unwind=no");
	if (info_.c_linkage)
		meta.push_back("linkage=c");
	meta.push_back(info_.weak ? "binding=weak"
	               : info_.internal ? "binding=internal"
	                                : "binding=strong");
	if (!info_.object_name.empty())
		meta.push_back("object=" + info_.object_name);
	if (info_.is_main)
		meta.push_back("keep_alias=yes");
	string metadata;
	for (size_t i = 0; i < meta.size(); i++)
		metadata += (i ? ", " : " [") + meta[i];
	metadata += "]";
	string ret_text;
	LowerAbiReturn(return_type_, ret_text);
	return "function @" + info_.low_name + "(" + params + ") -> " +
		ret_text + metadata + " {";
}

void FunctionLowerer::EmitParameterStores()
{
	OpenBlock("entry");
	size_t lead = indirect_ret_ ? 1 : 0;
	for (size_t i = lead; i < params_.size(); i++)
	{
		const SemNode& child = *def_.children[i - lead];
		const Scope* scope = child.entity_scope;
		TypePtr bare = RemoveTopCv(child.type);
		if (bare->kind == TK_CLASS)
		{
			// The named slot keeps the object's storage spelling; a
			// direct obj parameter copies into it, a by_address
			// parameter addresses the caller's object through %name.
			string slot = AddSlot(scope, params_[i].low_name,
			                      LowerSlotType(bare));
			if (params_[i].pass == "by_address")
			{
				address_aliases_[std::make_pair(
					(const void*)scope, child.name)] =
					"%" + params_[i].low_name;
				continue;
			}
			if (bare->named->class_record &&
			    bare->named->class_record->is_empty)
				continue;  // an empty object has no bytes to copy
			string address = NewTemp();
			Emit(address + " = addr $" + slot);
			Emit("copyobj " + LowerObjSpan(bare) + " %" +
			     params_[i].low_name + ", " + address);
			continue;
		}
		string slot = AddSlot(scope, params_[i].low_name,
		                      params_[i].type_text);
		Emit("store " + params_[i].type_text + " %" +
		     params_[i].low_name + ", $" + slot);
	}
}

// --- block / slot / temp state -----------------------------------------

string FunctionLowerer::NewTemp()
{
	string name;
	do
		name = "t" + to_string(++temp_counter_);
	while (param_names_.count(name));
	return "%" + name;
}

string FunctionLowerer::NewLabel(const string& prefix)
{
	return prefix + "_" + to_string(++label_counter_);
}

void FunctionLowerer::OpenBlock(const string& label)
{
	Block block;
	block.label = label;
	blocks_.push_back(block);
}

void FunctionLowerer::Emit(const string& line)
{
	Block& block = blocks_.back();
	if (block.terminated)
		return;  // unreachable straight-line code is dropped
	block.lines.push_back(line);
}

void FunctionLowerer::Terminate(const string& line)
{
	if (eh_open_)
		CloseEhRegion();
	Block& block = blocks_.back();
	if (block.terminated)
		return;
	block.lines.push_back(line);
	block.terminated = true;
}

void FunctionLowerer::ReferenceLabel(const string& label)
{
	referenced_.insert(label);
}

// Falls through into `label`: jumps from the current block when it is
// still open, then starts the new block.
void FunctionLowerer::CloseInto(const string& label)
{
	if (!blocks_.back().terminated)
	{
		ReferenceLabel(label);
		Terminate("jump ^" + label);
	}
	OpenBlock(label);
}

string FunctionLowerer::AddSlot(const Scope* scope, const string& name,
                                const string& type_text)
{
	int count = ++slot_base_counts_[name];
	string low = count == 1 ? name
	                        : name + "__shadow" + to_string(count);
	slots_.push_back(std::make_pair(low, type_text));
	slot_map_[std::make_pair((const void*)scope, name)] = low;
	return low;
}

string FunctionLowerer::AddMatSlot(const string& kind,
                                   const string& type_text)
{
	string low = kind + "__" + to_string(++mat_counter_);
	slots_.push_back(std::make_pair(low, type_text));
	return low;
}

string FunctionLowerer::SlotRef(const Scope* scope,
                                const string& name) const
{
	map<pair<const void*, string>, string>::const_iterator found =
		slot_map_.find(std::make_pair((const void*)scope, name));
	if (found == slot_map_.end())
		throw runtime_error("unlowered local " + name);
	return found->second;
}

void FunctionLowerer::TerminateOpenEnd()
{
	if (blocks_.back().terminated)
		return;
	Block& block = blocks_.back();
	if (block.lines.empty() && blocks_.size() > 1 &&
	    !referenced_.count(block.label))
	{
		blocks_.pop_back();
		return;
	}
	if (IsVoidType(return_type_))
	{
		Terminate("return void");
		return;
	}
	string type_text = LowerValueType(return_type_);
	if (LowerFloatType(return_type_))
		Terminate("return " + type_text + " " +
		          LowerFloatZero(return_type_));
	else if (type_text == "ptr")
	{
		string temp = NewTemp();
		Emit(temp + " = copy ptr nullptr");
		Terminate("return ptr " + temp);
	}
	else
		Terminate("return " + type_text + " 0");
}

string FunctionLowerer::Render() const
{
	string out = Header() + "\n";
	for (size_t i = 0; i < slots_.size(); i++)
		out += "  slot $" + slots_[i].first + " : " +
			slots_[i].second + "\n";
	if (!slots_.empty())
		out += "\n";
	for (size_t i = 0; i < blocks_.size(); i++)
	{
		if (i)
			out += "\n";
		out += "  block ^" + blocks_[i].label + ":\n";
		for (size_t j = 0; j < blocks_[i].lines.size(); j++)
			out += "    " + blocks_[i].lines[j] + "\n";
	}
	out += "}";
	return out;
}

// --- statements ----------------------------------------------------------

// Subobject construction inside synthesized bodies: an elided chain
// only demands its callee; value-initialization zero-fills through the
// constructor's address first.
void FunctionLowerer::LowerConstructorAction(const SemNode& node)
{
	if (node.trivial_init)
		return;
	if (node.elided)
	{
		program_.MemberFunctionRef(*node.children[0]->children[0]);
		return;
	}
	if (node.trivial_copy)
	{
		LowerTrivialCopyAction(node, "");
		return;
	}
	bool saved = in_lifetime_action_;
	in_lifetime_action_ = true;
	if (node.has_value && node.children[0]->children.size() > 1 &&
	    node.children[0]->children[1]->kind == SN_UNARY_EXPRESSION &&
	    node.children[0]->children[1]->op == OP_AMP)
	{
		string address = LowerAddressExpr(
			*node.children[0]->children[1]->children[0]);
		unsigned long long size = node.value.bits;
		string width;
		switch (size)
		{
		case 1: width = "i8"; break;
		case 2: width = "i16"; break;
		case 4: width = "i32"; break;
		case 8: width = "i64"; break;
		default:
			break;
		}
		if (width.empty())
			Emit("zeroinit " + to_string(size) + "x1 " + address);
		else
			Emit("store " + width + " 0, " + address);
		LowerConstructorCall(node, address);
		in_lifetime_action_ = saved;
		return;
	}
	LowerCall(*node.children[0]);
	in_lifetime_action_ = saved;
}

void FunctionLowerer::LowerStatementList(const vector<SemNodePtr>& items,
                                         size_t from)
{
	for (size_t i = from; i < items.size(); i++)
		LowerStatement(*items[i]);
}

void FunctionLowerer::LowerStatement(const SemNode& node)
{
	switch (node.kind)
	{
	case SN_COMPOUND_STATEMENT:
		PushCleanupScope();
		LowerStatementList(node.children, 0);
		PopCleanupScope(true);
		return;
	case SN_SIMPLE_DECLARATION:
		LowerLocalDeclaration(node);
		return;
	case SN_TYPE_ALIAS:
	case SN_FUNCTION_DECLARATION:
		return;  // no code; block-scope declares register on use
	case SN_VARIABLE:
		LowerLocalVariable(node);
		return;
	case SN_EXPRESSION_STATEMENT:
		BeginFullExpression(*node.children[0]);
		LowerEffect(*node.children[0]);
		EndFullExpression();
		return;
	case SN_CONSTRUCTOR_ACTION:
		LowerConstructorAction(node);
		return;
	case SN_STORAGE_COPY:
	{
		// A synthesized special member's leading trivially copyable
		// storage prefix: one raw object copy.
		string dst = LowerAddressExpr(*node.children[0]);
		string src = LowerAddressExpr(*node.children[1]);
		Emit("copyobj " + to_string(node.value.bits) + "x" +
		     to_string(node.bit_width) + " " + src + ", " + dst);
		return;
	}
	case SN_DESTRUCTOR_ACTION:
	{
		bool saved = in_lifetime_action_;
		in_lifetime_action_ = true;
		LowerCall(*node.children[0]);
		in_lifetime_action_ = saved;
		return;
	}
	case SN_DELETE_EXPRESSION:
	case SN_DELETE_ARRAY:
		LowerDelete(node);
		return;
	case SN_RETURN_STATEMENT:
		LowerReturn(node);
		return;
	case SN_IF_STATEMENT:
		LowerIf(node);
		return;
	case SN_WHILE_STATEMENT:
		LowerWhile(node);
		return;
	case SN_DO_STATEMENT:
		LowerDo(node);
		return;
	case SN_FOR_STATEMENT:
		LowerFor(node);
		return;
	case SN_SWITCH_STATEMENT:
		LowerSwitch(node);
		return;
	case SN_CASE_STATEMENT:
	case SN_DEFAULT_STATEMENT:
	{
		map<const SemNode*, string>::const_iterator found =
			case_labels_.find(&node);
		if (found == case_labels_.end())
			throw runtime_error("case label outside its switch");
		CloseInto(found->second);
		LowerStatement(*node.children[node.kind == SN_CASE_STATEMENT
		                              ? 1 : 0]);
		return;
	}
	case SN_BREAK_STATEMENT:
		if (break_stack_.empty())
			throw runtime_error("break outside a loop or switch");
		EmitCleanupsFrom(break_cleanup_.back());
		ReferenceLabel(break_stack_.back());
		Terminate("jump ^" + break_stack_.back());
		return;
	case SN_CONTINUE_STATEMENT:
		if (continue_stack_.empty())
			throw runtime_error("continue outside a loop");
		EmitCleanupsFrom(continue_cleanup_.back());
		ReferenceLabel(continue_stack_.back());
		Terminate("jump ^" + continue_stack_.back());
		return;
	case SN_GOTO_STATEMENT:
		LowerGoto(node);
		return;
	case SN_LABEL_STATEMENT:
		LowerLabelStatement(node);
		return;
	default:
		throw OutsideBoundary("statement form");
	}
}

void FunctionLowerer::LowerLocalDeclaration(const SemNode& node)
{
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& item = *node.children[i];
		if (item.kind == SN_VARIABLE)
			LowerLocalVariable(item);
		else if (item.kind != SN_TYPE_ALIAS &&
		         item.kind != SN_FUNCTION_DECLARATION)
			throw OutsideBoundary("local declaration form");
	}
}

void FunctionLowerer::LowerLocalVariable(const SemNode& node)
{
	if (node.is_static_decl)
		throw OutsideBoundary("function-local static object");
	if (node.is_extern_decl && node.children.empty())
	{
		// A block-scope extern declaration names the namespace-scope
		// entity; no local storage exists.
		program_.RegisterGlobal(node);
		return;
	}
	const TypePtr& declared = node.type;
	{
		TypePtr inner = declared;
		while (inner->kind == TK_ARRAY)
			inner = inner->target;
		if (RemoveTopCv(inner)->kind == TK_CLASS &&
		    !IsReferenceType(declared))
		{
			BeginFullExpression(node);
			LowerClassLocal(node);
			EndFullExpression();
			// The object's own scope cleanup arms only once its
			// initialization completed.
			if (!pending_cleanup_.empty())
			{
				RegisterCleanup(pending_cleanup_);
				pending_cleanup_.clear();
			}
			return;
		}
	}
	if (IsReferenceType(declared))
	{
		string slot = AddSlot(node.entity_scope, node.entity_name,
		                      "ptr");
		BeginFullExpression(node);
		string address = LowerAddressExpr(*node.children[0]);
		Emit("store ptr " + address + ", $" + slot);
		EndFullExpression();
		return;
	}
	if (declared->kind == TK_ARRAY)
	{
		string slot = AddSlot(node.entity_scope, node.entity_name,
		                      LowerSlotType(declared));
		if (node.children.empty())
			return;
		if (node.children[0]->kind != SN_BRACED_INIT_LIST)
			throw OutsideBoundary("array initializer form");
		LowerLocalArrayInit(*node.children[0], slot, node.type);
		return;
	}
	string slot = AddSlot(node.entity_scope, node.entity_name,
	                      LowerSlotType(declared));
	if (node.children.empty())
		return;
	BeginFullExpression(node);
	string value = LowerValueAs(*node.children[0],
	                            RemoveTopCv(declared), LCC_INIT);
	Emit("store " + LowerSlotType(declared) + " " + value + ", $" +
	     slot);
	EndFullExpression();
}

void FunctionLowerer::LowerLocalArrayInit(const SemNode& node,
                                          const string& slot,
                                          const TypePtr& array)
{
	TypePtr element = RemoveTopCv(array->target);
	unsigned long long size = TypeSize(element);
	string base = NewTemp();
	Emit(base + " = addr $" + slot);
	for (size_t i = 0; i < node.children.size(); i++)
	{
		string target = base;
		if (i)
		{
			target = NewTemp();
			Emit(target + " = index i8 " + base + ", " +
			     to_string(i * size));
		}
		string value = LowerValueAs(*node.children[i], element,
		                            LCC_INIT);
		Emit("store " + LowerValueType(element) + " " + value + ", " +
		     target);
	}
	if (array->bound > node.children.size())
	{
		unsigned long long offset = node.children.size() * size;
		string target = base;
		if (offset)
		{
			target = NewTemp();
			Emit(target + " = index i8 " + base + ", " +
			     to_string(offset));
		}
		Emit("zeroinit " +
		     to_string((array->bound - node.children.size()) * size) +
		     "x" + to_string(TypeAlignment(element)) + " " + target);
	}
}

// The single source local of a class-valued return statement's
// copy-initialization, or null.
static const SemNode* ReturnSourceLocal(const SemNode& ret)
{
	if (ret.children.empty())
		return 0;
	const SemNode& child = *ret.children[0];
	if (child.kind != SN_CONSTRUCTOR_ACTION || !child.synth_copy)
		return 0;
	const SemNode& call = *child.children[0];
	const SemNode& source = *call.children.back();
	if (source.kind != SN_ID_EXPRESSION || !source.entity_scope)
		return 0;
	return &source;
}

void FunctionLowerer::CollectReturnLocals(const SemNode& node,
                                          bool& eligible,
                                          const SemNode*& source)
{
	if (node.kind == SN_RETURN_STATEMENT && !node.children.empty())
	{
		const SemNode* named = ReturnSourceLocal(node);
		if (!named)
		{
			eligible = false;
			return;
		}
		if (source && (source->entity_scope != named->entity_scope ||
		               source->entity_name != named->entity_name))
		{
			eligible = false;
			return;
		}
		source = named;
		return;
	}
	for (size_t i = 0; i < node.children.size(); i++)
		CollectReturnLocals(*node.children[i], eligible, source);
}

// The PA16 return-slot-reuse form: when every class-valued return
// names the same top-level local (declared directly in the function
// body), that local lives in %ret and the return copies elide.
void FunctionLowerer::ScanReturnSlotReuse()
{
	bool eligible = true;
	const SemNode* source = 0;
	CollectReturnLocals(def_, eligible, source);
	if (!eligible || !source)
		return;
	// The named local must be a top-level declaration of the body.
	const SemNode* body = 0;
	for (size_t i = 0; i < def_.children.size(); i++)
		if (def_.children[i]->kind == SN_COMPOUND_STATEMENT)
		{
			body = def_.children[i].get();
			break;
		}
	if (!body)
		return;
	for (size_t i = 0; i < body->children.size(); i++)
	{
		const SemNode* item = body->children[i].get();
		if (item->kind == SN_SIMPLE_DECLARATION &&
		    item->children.size() == 1)
			item = item->children[0].get();
		if (item->kind != SN_VARIABLE)
			continue;
		if (item->entity_scope == source->entity_scope &&
		    item->entity_name == source->entity_name &&
		    !IsReferenceType(item->type) &&
		    TypeEquals(RemoveTopCv(item->type),
		               RemoveTopCv(return_type_)))
		{
			nrvo_scope_ = source->entity_scope;
			nrvo_name_ = source->entity_name;
			return;
		}
	}
}

void FunctionLowerer::LowerReturn(const SemNode& node)
{
	if (node.children.empty())
	{
		EmitCleanupsFrom(0);
		Terminate("return void");
		return;
	}
	const SemNode& value = *node.children[0];
	TypePtr ret_bare = RemoveTopCv(return_type_);
	if (!IsReferenceType(return_type_) && ret_bare->kind == TK_CLASS)
	{
		BeginFullExpression(value);
		if (indirect_ret_)
		{
			const SemNode* named = ReturnSourceLocal(node);
			bool reused = nrvo_active_ && named &&
				named->entity_scope == nrvo_scope_ &&
				named->entity_name == nrvo_name_;
			if (!reused)
				LowerClassInit(value, "%ret");
			EndFullExpression();
			EmitCleanupsFrom(0);
			Terminate("return void");
			return;
		}
		string slot = AddMatSlot("retobj", LowerSlotType(ret_bare));
		string address = NewTemp();
		Emit(address + " = addr $" + slot);
		LowerClassInit(value, address);
		EndFullExpression();
		EmitCleanupsFrom(0);
		Terminate("return " + LowerSlotType(ret_bare) + " $" + slot);
		return;
	}
	BeginFullExpression(value);
	if (IsVoidType(return_type_))
	{
		LowerEffect(value);
		EndFullExpression();
		EmitCleanupsFrom(0);
		Terminate("return void");
		return;
	}
	if (IsReferenceType(return_type_))
	{
		string address = LowerAddressExpr(value);
		TypePtr source = value.type;
		if (IsReferenceType(source))
			source = source->target;
		source = RemoveTopCv(source);
		TypePtr referee = RemoveTopCv(return_type_->target);
		if (source->kind == TK_CLASS && referee->kind == TK_CLASS)
		{
			int hops = BaseClassDistance(source->named, referee->named);
			for (int i = 0; i < hops; i++)
			{
				string hopped = NewTemp();
				Emit(hopped +
				     " = index i8 [projection=base_subobject] " +
				     address + ", 0");
				address = hopped;
			}
		}
		EndFullExpression();
		EmitCleanupsFrom(0);
		Terminate("return ptr " + address);
		return;
	}
	string text = LowerValueAs(value, RemoveTopCv(return_type_),
	                           LCC_INIT);
	EndFullExpression();
	EmitCleanupsFrom(0);
	Terminate("return " + LowerValueType(return_type_) + " " + text);
}

void FunctionLowerer::LowerIf(const SemNode& node)
{
	string then_label = NewLabel("if_then");
	string else_label = NewLabel("if_else");
	string end_label = NewLabel("if_end");
	LowerConditionInto(*node.children[0], then_label, else_label);
	OpenBlock(then_label);
	LowerStatement(*node.children[1]->children[0]);
	if (!blocks_.back().terminated)
	{
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
	}
	OpenBlock(else_label);
	if (node.children.size() > 2)
		LowerStatement(*node.children[2]->children[0]);
	if (!blocks_.back().terminated)
	{
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
	}
	OpenBlock(end_label);
}

void FunctionLowerer::LowerWhile(const SemNode& node)
{
	string cond_label = NewLabel("while_cond");
	string body_label = NewLabel("while_body");
	string end_label = NewLabel("while_end");
	CloseInto(cond_label);
	LowerConditionInto(*node.children[0], body_label, end_label);
	OpenBlock(body_label);
	break_stack_.push_back(end_label);
	continue_stack_.push_back(cond_label);
	break_cleanup_.push_back(cleanup_scopes_.size());
	continue_cleanup_.push_back(cleanup_scopes_.size());
	LowerStatement(*node.children[1]);
	break_stack_.pop_back();
	continue_stack_.pop_back();
	break_cleanup_.pop_back();
	continue_cleanup_.pop_back();
	if (!blocks_.back().terminated)
	{
		ReferenceLabel(cond_label);
		Terminate("jump ^" + cond_label);
	}
	OpenBlock(end_label);
}

void FunctionLowerer::LowerDo(const SemNode& node)
{
	string body_label = NewLabel("do_body");
	string cond_label = NewLabel("do_cond");
	string end_label = NewLabel("do_end");
	CloseInto(body_label);
	break_stack_.push_back(end_label);
	continue_stack_.push_back(cond_label);
	break_cleanup_.push_back(cleanup_scopes_.size());
	continue_cleanup_.push_back(cleanup_scopes_.size());
	LowerStatement(*node.children[0]);
	break_stack_.pop_back();
	continue_stack_.pop_back();
	break_cleanup_.pop_back();
	continue_cleanup_.pop_back();
	CloseInto(cond_label);
	LowerConditionInto(*node.children[1], body_label, end_label);
	OpenBlock(end_label);
}

void FunctionLowerer::LowerFor(const SemNode& node)
{
	size_t next = 0;
	if (next < node.children.size() &&
	    node.children[next]->kind == SN_FOR_INIT)
	{
		LowerStatement(*node.children[next]->children[0]);
		next++;
	}
	const SemNode* condition = 0;
	if (next < node.children.size() &&
	    node.children[next]->kind == SN_CONDITION)
		condition = node.children[next++].get();
	const SemNode* iteration = 0;
	if (next < node.children.size() &&
	    node.children[next]->kind == SN_ITERATION)
		iteration = node.children[next++].get();
	string cond_label = NewLabel("for_cond");
	string body_label = NewLabel("for_body");
	string iter_label = NewLabel("for_iter");
	string end_label = NewLabel("for_end");
	CloseInto(cond_label);
	if (condition)
		LowerConditionInto(*condition, body_label, end_label);
	else
	{
		ReferenceLabel(body_label);
		Terminate("jump ^" + body_label);
	}
	OpenBlock(body_label);
	break_stack_.push_back(end_label);
	continue_stack_.push_back(iter_label);
	break_cleanup_.push_back(cleanup_scopes_.size());
	continue_cleanup_.push_back(cleanup_scopes_.size());
	LowerStatement(*node.children[next]);
	break_stack_.pop_back();
	continue_stack_.pop_back();
	break_cleanup_.pop_back();
	continue_cleanup_.pop_back();
	CloseInto(iter_label);
	if (iteration)
		LowerEffect(*iteration->children[0]);
	if (!blocks_.back().terminated)
	{
		ReferenceLabel(cond_label);
		Terminate("jump ^" + cond_label);
	}
	OpenBlock(end_label);
}

void FunctionLowerer::ScanSwitchLabels(const SemNode& node,
                                       vector<string>& arms,
                                       string& default_label)
{
	if (node.kind == SN_SWITCH_STATEMENT)
		return;  // nested switch labels belong to that switch
	if (node.kind == SN_CASE_STATEMENT)
	{
		string label = NewLabel("switch_case");
		case_labels_[&node] = label;
		arms.push_back(RenderConstValue(node.value) + ":^" + label);
		ReferenceLabel(label);
		ScanSwitchLabels(*node.children[1], arms, default_label);
		return;
	}
	if (node.kind == SN_DEFAULT_STATEMENT)
	{
		default_label = NewLabel("switch_default");
		case_labels_[&node] = default_label;
		ReferenceLabel(default_label);
		ScanSwitchLabels(*node.children[0], arms, default_label);
		return;
	}
	for (size_t i = 0; i < node.children.size(); i++)
		ScanSwitchLabels(*node.children[i], arms, default_label);
}

void FunctionLowerer::LowerSwitch(const SemNode& node)
{
	const SemNode& condition = *node.children[0];
	string selector;
	if (condition.children[0]->kind == SN_CONDITION_DECLARATION)
	{
		const SemNode& inner = *condition.children[0];
		const SemNode& variable = *inner.children[0];
		LowerLocalVariable(variable);
		if (inner.children.size() > 1)
			selector = LowerValueExpr(*inner.children[1]).text;
		else
		{
			TypePtr type = StripReference(variable.type);
			selector = NewTemp();
			Emit(selector + " = load " + LowerValueType(type) + " $" +
			     SlotRef(variable.entity_scope, variable.entity_name));
		}
	}
	else
		selector = LowerValueExpr(*condition.children[0]).text;
	string dispatch_label = NewLabel("switch_dispatch");
	string end_label = NewLabel("switch_end");
	vector<string> arms;
	string default_label;
	ScanSwitchLabels(*node.children[1], arms, default_label);
	CloseInto(dispatch_label);
	string target = default_label.empty() ? end_label : default_label;
	ReferenceLabel(target);
	string line = "switch " + selector + ", ^" + target;
	for (size_t i = 0; i < arms.size(); i++)
		line += ", " + arms[i];
	Terminate(line);
	break_stack_.push_back(end_label);
	break_cleanup_.push_back(cleanup_scopes_.size());
	LowerStatement(*node.children[1]);
	break_stack_.pop_back();
	break_cleanup_.pop_back();
	CloseInto(end_label);
}

string FunctionLowerer::GotoLabel(const string& name)
{
	map<string, string>::const_iterator found = goto_labels_.find(name);
	if (found != goto_labels_.end())
		return found->second;
	string label = NewLabel("goto");
	goto_labels_[name] = label;
	return label;
}

void FunctionLowerer::LowerGoto(const SemNode& node)
{
	string label = GotoLabel(node.name);
	ReferenceLabel(label);
	Terminate("jump ^" + label);
}

void FunctionLowerer::LowerLabelStatement(const SemNode& node)
{
	CloseInto(GotoLabel(node.name));
	LowerStatement(*node.children[0]);
}

void FunctionLowerer::LowerConditionInto(const SemNode& condition,
                                         const string& true_label,
                                         const string& false_label)
{
	const SemNode& inner = *condition.children[0];
	if (inner.kind != SN_CONDITION_DECLARATION)
	{
		LowerCondition(inner, true_label, false_label);
		return;
	}
	// 6.4p4: the condition value is the declared variable, read back
	// from its slot after initialization (a class variable converts
	// through its attached conversion-function call).
	const SemNode& variable = *inner.children[0];
	LowerLocalVariable(variable);
	LowerValue value;
	if (inner.children.size() > 1)
		value = LowerValueExpr(*inner.children[1]);
	else
	{
		TypePtr type = StripReference(variable.type);
		value.text = NewTemp();
		value.type = RemoveTopCv(type);
		Emit(value.text + " = load " + LowerValueType(type) + " $" +
		     SlotRef(variable.entity_scope, variable.entity_name));
	}
	LowerValue truth = MaterializeTruth(value);
	ReferenceLabel(true_label);
	ReferenceLabel(false_label);
	Terminate("branch " + truth.text + ", ^" + true_label + ", ^" +
	          false_label);
}
