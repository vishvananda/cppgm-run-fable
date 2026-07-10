#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;
using std::to_string;

// PA24 lambda expressions (5.1.2) over the assignment subset:
// captureless lambdas synthesize an internal function whose name the
// expression yields (it decays like an ordinary function lvalue);
// capturing lambdas synthesize a closure class whose fields hold the
// by-reference captures (and the captured `this` pointer), whose
// `operator ()` carries the body, and whose construction stores the
// capture addresses field-wise at the use site.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA24 assignment boundary");
}

TypePtr LambdaAutoPlaceholder()
{
	Type marked;
	marked.kind = TK_FUNDAMENTAL;
	marked.fundamental = FT_VOID;
	marked.is_auto_placeholder = true;
	return std::make_shared<Type>(marked);
}

// Whether a statement subtree declares a class (a lambda whose body
// does keeps the closure view under auto deduction - the reference
// shape for local-type-owning lambdas).
bool StmtDeclaresClass(const AstStmt* stmt)
{
	if (!stmt)
		return false;
	if (stmt->decl && (stmt->decl->kind == DK_CLASS ||
	                   stmt->decl->kind == DK_SIMPLE))
	{
		if (stmt->decl->kind == DK_CLASS)
			return true;
		for (size_t i = 0; i < stmt->decl->specifiers.size(); i++)
			if (stmt->decl->specifiers[i].kind == SPEC_NESTED_DECL &&
			    stmt->decl->specifiers[i].nested_decl &&
			    stmt->decl->specifiers[i].nested_decl->kind == DK_CLASS)
				return true;
	}
	for (size_t i = 0; i < stmt->items.size(); i++)
		if (StmtDeclaresClass(stmt->items[i].get()))
			return true;
	if (StmtDeclaresClass(stmt->then_branch.get()) ||
	    StmtDeclaresClass(stmt->else_branch.get()) ||
	    StmtDeclaresClass(stmt->body.get()) ||
	    StmtDeclaresClass(stmt->for_init.get()))
		return true;
	return false;
}

}  // namespace

// The innermost lambda frame whose body is the one currently being
// bound; a nested deferred body (a local class member) switches
// method_.fn_scope and disables the enclosing frame until it returns.
SemBinder::LambdaFrame* SemBinder::ActiveLambdaFrame()
{
	for (size_t i = lambda_frames_.size(); i > 0; i--)
		if (lambda_frames_[i - 1].fn_scope == method_.fn_scope)
			return &lambda_frames_[i - 1];
	return 0;
}

// The closure operator()'s own object lvalue (`*this`), the base of
// every capture-field access.
SemNodePtr SemBinder::ClosureThisId(const LambdaFrame& frame)
{
	SemNodePtr id = MakeSemNode(SN_ID_EXPRESSION);
	id->name = "this";
	id->type = frame.this_param_type;
	id->category = VC_PRVALUE;
	id->entity_scope = frame.fn_scope;
	id->entity_name = "this";
	SemNodePtr deref = MakeSemNode(SN_UNARY_EXPRESSION);
	deref->type = frame.this_param_type->target;
	deref->category = VC_LVALUE;
	deref->has_op = true;
	deref->op = OP_STAR;
	deref->op_spelling = "*";
	deref->children.push_back(std::move(id));
	return deref;
}

// The captured-this pointer field (created on the spelled capture or
// the first use).
void SemBinder::EnsureThisField(LambdaFrame& frame)
{
	if (frame.this_captured)
		return;
	ClassField field;
	field.name = "__this";
	field.type = frame.enclosing_this;
	field.access = MA_PUBLIC;
	ClassField& placed = LayoutField(*frame.cls, field);
	frame.this_captured = true;
	frame.this_offset = placed.offset;
	LambdaCapture capture;
	capture.is_this = true;
	frame.captures.push_back(capture);
}

// The capture field of one enclosing entity (created on the spelled
// capture or the first use); returns its capture index. A by-copy
// capture lays out a value field, a by-reference one a reference
// field.
size_t SemBinder::EnsureCaptureField(LambdaFrame& frame,
                                     const ScopeBinding& binding,
                                     bool by_copy)
{
	for (size_t i = 0; i < frame.captures.size(); i++)
		if (frame.captures[i].binding == &binding)
			return i;
	TypePtr referee = binding.type;
	if (IsReferenceType(referee))
		referee = referee->target;
	if (by_copy)
		referee = RemoveTopCv(referee);
	ClassField field;
	field.name = binding.name;
	field.type = by_copy ? referee
	                     : MakeReferenceType(referee, false, false);
	field.access = MA_PUBLIC;
	if (by_copy && RemoveTopCv(referee)->kind == TK_CLASS)
		RequireCompleteType(RemoveTopCv(referee)->named);
	ClassField& placed = LayoutField(*frame.cls, field);
	LambdaCapture capture;
	capture.binding = &binding;
	capture.name = binding.name;
	capture.offset = placed.offset;
	capture.referee = referee;
	capture.by_copy = by_copy;
	frame.captures.push_back(capture);
	return frame.captures.size() - 1;
}

SemNodePtr SemBinder::ThisValueNode()
{
	LambdaFrame* frame = ActiveLambdaFrame();
	if (frame && frame->cls && frame->enclosing_this)
	{
		// 5.1.2p17-adjacent: `this` inside the lambda body reads the
		// captured pointer field.
		if (!frame->this_captured)
		{
			if (!frame->by_ref_default && !frame->by_copy_default &&
			    !frame->this_spelled)
				throw runtime_error("this is not captured");
			EnsureThisField(*frame);
		}
		SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
		member->name = "__this";
		member->type = frame->enclosing_this;
		member->category = VC_PRVALUE;
		member->member_offset = frame->this_offset;
		member->children.push_back(ClosureThisId(*frame));
		return member;
	}
	// The ordinary parameter-backed id (the `this` binding lives in
	// the enclosing function scope).
	SemNodePtr id = MakeSemNode(SN_ID_EXPRESSION);
	id->name = "this";
	id->type = CurrentThisType();
	id->category = VC_PRVALUE;
	id->entity_scope = current_;
	for (const Scope* scope = current_; scope; scope = scope->parent)
		if (FindOwnBinding(*scope, "this"))
		{
			id->entity_scope = scope;
			break;
		}
	id->entity_name = "this";
	return id;
}

bool SemBinder::TryCaptureUse(const ScopeBinding& binding, SemValue& out)
{
	LambdaFrame* frame = ActiveLambdaFrame();
	if (!frame)
		return false;
	// Only enclosing function-local objects capture: the lambda's own
	// locals and parameters live under its function scope, and
	// namespace/class-scope entities resolve directly.
	if (!binding.home || (binding.home->kind != SCOPE_FUNCTION &&
	                      binding.home->kind != SCOPE_BLOCK))
		return false;
	for (const Scope* scope = binding.home; scope; scope = scope->parent)
		if (scope == frame->fn_scope)
			return false;
	if (!frame->cls)
		throw runtime_error(binding.name + " is not captured by the "
		                    "captureless lambda");
	bool spelled = false;
	bool by_copy = frame->by_copy_default;
	for (size_t i = 0; i < frame->explicit_names.size(); i++)
		if (frame->explicit_names[i] == binding.name)
		{
			spelled = true;
			by_copy = frame->explicit_copy[i] != 0;
		}
	if (!frame->by_ref_default && !frame->by_copy_default && !spelled)
		throw runtime_error(binding.name + " is not captured");
	size_t index = EnsureCaptureField(*frame, binding, by_copy);
	const LambdaCapture& capture = frame->captures[index];
	out = SemValue();
	out.type = capture.referee;
	out.category = VC_LVALUE;
	out.node = MakeSemNode(SN_MEMBER_EXPRESSION);
	out.node->name = capture.name;
	out.node->type = capture.referee;
	out.node->member_ref = !capture.by_copy;
	out.node->category = VC_LVALUE;
	out.node->member_offset = capture.offset;
	out.node->children.push_back(ClosureThisId(*frame));
	return true;
}

bool SemBinder::CapturelessClosureFunction(const NamedTypeInfo* cls,
                                           const Scope*& owner,
                                           string& name, TypePtr& type)
{
	std::map<const NamedTypeInfo*, ClosureFunction>::const_iterator
		found = closure_functions_.find(cls);
	if (found == closure_functions_.end())
		return false;
	owner = found->second.owner;
	name = found->second.name;
	type = found->second.type;
	return true;
}

// Rebuilds the expression value of an already-synthesized lambda: the
// function name for a captureless one, a closure-construction value
// (capture sources re-analyzed in the current context) otherwise.
SemValue SemBinder::MakeLambdaValue(const LambdaInfo& info)
{
	SemValue value;
	TypePtr class_type = MakeNamedType(TK_CLASS, info.cls->entity);
	value.type = class_type;
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_CLOSURE_INIT);
	value.node->type = class_type;
	value.node->category = VC_PRVALUE;
	for (size_t i = 0; i < info.captures.size(); i++)
	{
		AstExprPtr source;
		if (info.captures[i].is_this)
		{
			source.reset(new AstExpr(EK_KEYWORD_LITERAL));
			source->op = KW_THIS;
			source->literal = "this";
		}
		else
		{
			source.reset(new AstExpr(EK_ID));
			AstNamePart part;
			part.kind = NP_IDENTIFIER;
			part.identifier = info.captures[i].name;
			source->name.parts.push_back(std::move(part));
		}
		SemValue capture = analyzer_.Analyze(*source);
		synth_exprs_.push_back(std::move(source));
		if (info.captures[i].by_copy && capture.type &&
		    RemoveTopCv(capture.type)->kind == TK_CLASS)
		{
			// PA25: a by-copy class capture copy-initializes its
			// field from the enclosing object.
			TypePtr bare = RemoveTopCv(capture.type);
			const ClassInfo* cls = unit_.classes.Find(bare->named);
			if (!cls)
				throw runtime_error("captured class record missing");
			vector<SemValue> args;
			args.push_back(std::move(capture));
			int index = ResolveClassConstructor(*cls, args, true,
			                                    "capture");
			vector<SemNodePtr> arg_nodes;
			for (size_t a = 0; a < args.size(); a++)
				arg_nodes.push_back(std::move(args[a].node));
			SemNodePtr action = MakeConstructorCall(
				*cls, index, false, SemNodePtr(),
				std::move(arg_nodes));
			action->type = bare;
			action->category = VC_PRVALUE;
			value.node->children.push_back(std::move(action));
			continue;
		}
		value.node->children.push_back(std::move(capture.node));
	}
	return value;
}

SemValue SemBinder::AnalyzeLambda(const AstExpr& expr)
{
	const AstLambda& lambda = *expr.lambda;
	// One synthesis per (lambda, enclosing body): the deduction and
	// initialization analyses of one declaration share it, while a
	// re-instantiated template body synthesizes its own.
	std::pair<const void*, const void*> key(&lambda, method_.fn_scope);
	std::map<std::pair<const void*, const void*>,
	         LambdaInfo>::iterator found = lambda_cache_.find(key);
	if (found != lambda_cache_.end())
		return MakeLambdaValue(found->second);
	for (size_t i = 0; i < lambda.captures.size(); i++)
		if (lambda.captures[i].pack)
			throw OutsideBoundary("pack capture");
	bool captureless = !lambda.has_capture_default &&
		lambda.captures.empty();

	++lambda_counter_;
	string name = "__lambda" + to_string(lambda_counter_);
	if (!method_.fn_name.empty())
	{
		// The closure-class name: the enclosing function's qualified
		// stem plus the lambda-declarator's token span.
		string qualified = method_.fn_name;
		for (const Scope* scope = method_.fn_owner; scope;
		     scope = scope->parent)
			if (scope->kind == SCOPE_CLASS && !scope->name.empty())
				qualified = scope->name + "::" + qualified;
		string stem;
		for (size_t i = 0; i < qualified.size(); i++)
		{
			char c = qualified[i];
			bool word = (c >= 'a' && c <= 'z') ||
				(c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
				c == '_';
			if (word)
				stem += c;
			else if (c == '<')
				stem += "__";
			else if (c == ':' && i + 1 < qualified.size() &&
			         qualified[i + 1] == ':')
			{
				stem += "__";
				i++;
			}
			else
				stem += '_';
		}
		if (stem.empty() || stem[stem.size() - 1] != '_')
			stem += '_';
		name = "__lambda_" + stem + "t" +
			to_string(lambda.declarator_begin_token) + "_" +
			to_string(lambda.body_begin_token);
	}

	// The operator's scope chains to the surrounding context so
	// enclosing types, template parameters, and globals resolve.
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, name, current_);
	vector<ParameterInfo> parameters;
	vector<TypePtr> param_types;
	// Parameters (and an expanded pack's body-visible binding) publish
	// into the lambda's own scope while they compose.
	Scope* saved_capture = param_capture_scope_;
	param_capture_scope_ = fn_scope;
	PackParamRecord saved_record = last_pack_param_;
	last_pack_param_ = PackParamRecord();
	try
	{
		if (lambda.has_declarator && lambda.parameters)
			builder_.BuildParameters(*lambda.parameters, parameters,
			                         param_types, 0);
		BindCapturedPackParameter(fn_scope);
	}
	catch (...)
	{
		param_capture_scope_ = saved_capture;
		last_pack_param_ = saved_record;
		throw;
	}
	param_capture_scope_ = saved_capture;
	last_pack_param_ = saved_record;
	for (size_t i = 0; i < parameters.size(); i++)
	{
		if (parameters[i].name.empty() ||
		    FindOwnBinding(*fn_scope, parameters[i].name))
			continue;
		ScopeBinding param_binding;
		param_binding.kind = SB_PARAMETER;
		param_binding.name = parameters[i].name;
		param_binding.type = parameters[i].type;
		AddBinding(*fn_scope, param_binding);
	}
	// 5.1.2p4: the trailing-return-type resolves with the parameters
	// in scope (a decltype may name them); no trailing type leaves the
	// deducible placeholder.
	TypePtr ret = LambdaAutoPlaceholder();
	if (lambda.has_trailing)
	{
		Scope* saved = current_;
		current_ = fn_scope;
		try
		{
			ret = builder_.ResolveTypeId(*lambda.trailing_type);
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
	LambdaInfo info;
	info.captureless = captureless;
	if (captureless)
		BindCapturelessLambda(lambda, name, fn_scope, parameters,
		                      param_types, ret, info);
	else
		BindClosureLambda(lambda, name, fn_scope, parameters,
		                  param_types, ret, info);
	LambdaInfo& cached = lambda_cache_[key];
	cached = info;
	return MakeLambdaValue(cached);
}

// Binds a lambda body as the open function: the shared statement path
// runs with the given method context, return deduction included.
// Returns the deduced return type.
TypePtr SemBinder::BindLambdaBody(const AstLambda& lambda, SemNode* node,
                                  Scope* fn_scope,
                                  const MethodContext& context,
                                  const TypePtr& ret)
{
	Scope* saved_scope = current_;
	MethodContext saved_method = method_;
	TypePtr saved_return = current_return_;
	TypePtr saved_pattern = auto_return_pattern_;
	int saved_hidden = range_hidden_counter_;
	vector<SemNode*> saved_parents;
	saved_parents.swap(parents_);
	current_ = fn_scope;
	method_ = context;
	current_return_ = ret;
	auto_return_pattern_ = TypeContainsAutoPlaceholder(ret)
		? ret : TypePtr();
	range_hidden_counter_ = 0;
	parents_.push_back(node);
	try
	{
		BindStatement(*lambda.body);
	}
	catch (...)
	{
		parents_.swap(saved_parents);
		current_ = saved_scope;
		method_ = saved_method;
		current_return_ = saved_return;
		auto_return_pattern_ = saved_pattern;
		range_hidden_counter_ = saved_hidden;
		throw;
	}
	TypePtr deduced = current_return_;
	parents_.swap(saved_parents);
	current_ = saved_scope;
	method_ = saved_method;
	current_return_ = saved_return;
	auto_return_pattern_ = saved_pattern;
	range_hidden_counter_ = saved_hidden;
	if (TypeContainsAutoPlaceholder(deduced))
		deduced = MakeFundamentalType(FT_VOID);
	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	node->unwind_no = !may_throw;
	return deduced;
}

void SemBinder::BindCapturelessLambda(const AstLambda& lambda,
                                      const string& name, Scope* fn_scope,
                                      const vector<ParameterInfo>& parameters,
                                      const vector<TypePtr>& param_types,
                                      const TypePtr& ret, LambdaInfo& info)
{
	// The captureless closure is an empty class whose operator ()
	// carries the body; the same body also synthesizes as an internal
	// free function - the target of the closure's function-pointer
	// conversion, of auto deduction, and of direct expression calls.
	// Each form emits only on demand.
	TypePtr fn_type = MakeFunctionType(ret, param_types, false);
	fn_scope->fn_type = fn_type;
	SemNodePtr item = MakeSemNode(SN_FUNCTION_DEFINITION);
	SemNode* node = item.get();
	node->name = name;
	node->type = fn_type;
	node->entity_scope = model_.global();
	node->entity_name = name;
	node->internal_fn = true;
	for (size_t i = 0; i < parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = parameters[i].name;
		parameter->type = parameters[i].type;
		parameter->entity_scope = fn_scope;
		parameter->entity_name = parameters[i].name;
		node->children.push_back(std::move(parameter));
	}
	MethodContext context;
	context.fn_scope = fn_scope;
	context.fn_owner = model_.global();
	context.fn_name = name;
	// Access rights follow the lexical context (11.7): a lambda in a
	// member function reaches the class's private names.
	context.lexical_cls = method_.cls ? method_.cls : method_.lexical_cls;
	LambdaFrame frame;
	frame.fn_scope = fn_scope;
	lambda_frames_.push_back(frame);
	TypePtr deduced;
	try
	{
		deduced = BindLambdaBody(lambda, node, fn_scope, context, ret);
	}
	catch (...)
	{
		lambda_frames_.pop_back();
		throw;
	}
	lambda_frames_.pop_back();
	if (TypeContainsAutoPlaceholder(ret))
	{
		fn_type = MakeFunctionType(deduced, param_types, false);
		node->type = fn_type;
		fn_scope->fn_type = fn_type;
	}
	ScopeBinding binding;
	binding.kind = SB_FUNCTION;
	binding.name = name;
	binding.type = fn_type;
	ScopeBinding& added = AddBinding(*model_.global(), binding);
	if (node->unwind_no)
	{
		added.fn_unwind_no.resize(1, false);
		added.fn_unwind_no[0] = true;
	}
	unit_.deferred.push_back(std::move(item));
	info.fn_name = name;
	info.fn_type = fn_type;
	// The closure class and its operator () (a second synthesis of the
	// same body in its own scope).
	Scope* op_scope = model_.CreateScope(SCOPE_FUNCTION, name, current_);
	{
		Scope* saved_capture = param_capture_scope_;
		param_capture_scope_ = op_scope;
		PackParamRecord saved_record = last_pack_param_;
		last_pack_param_ = PackParamRecord();
		vector<ParameterInfo> op_parameters;
		vector<TypePtr> op_param_types;
		try
		{
			if (lambda.has_declarator && lambda.parameters)
				builder_.BuildParameters(*lambda.parameters,
				                         op_parameters,
				                         op_param_types, 0);
			BindCapturedPackParameter(op_scope);
		}
		catch (...)
		{
			param_capture_scope_ = saved_capture;
			last_pack_param_ = saved_record;
			throw;
		}
		param_capture_scope_ = saved_capture;
		last_pack_param_ = saved_record;
		for (size_t i = 0; i < op_parameters.size(); i++)
		{
			if (op_parameters[i].name.empty() ||
			    FindOwnBinding(*op_scope, op_parameters[i].name))
				continue;
			ScopeBinding param_binding;
			param_binding.kind = SB_PARAMETER;
			param_binding.name = op_parameters[i].name;
			param_binding.type = op_parameters[i].type;
			AddBinding(*op_scope, param_binding);
		}
		BindClosureLambda(lambda, name, op_scope, op_parameters,
		                  op_param_types, ret, info);
	}
	closure_functions_[info.cls->entity].owner = model_.global();
	closure_functions_[info.cls->entity].name = name;
	closure_functions_[info.cls->entity].type = fn_type;
	if (StmtDeclaresClass(lambda.body.get()))
		closure_object_view_.insert(info.cls->entity);
}

void SemBinder::BindClosureLambda(const AstLambda& lambda,
                                  const string& name, Scope* fn_scope,
                                  const vector<ParameterInfo>& parameters,
                                  const vector<TypePtr>& param_types,
                                  const TypePtr& ret, LambdaInfo& info)
{
	// The closure class: a unique local class whose fields appear as
	// the body captures.
	NamedTypeInfo* entity = model_.CreateNamedTypeInfo(
		"class " + name, current_, name);
	entity->class_key = "class";
	entity->is_closure = true;
	{
		// 5.1.7: the <lambda-sig> discriminator counts earlier
		// lambdas with the same signature in the same context.
		std::vector<std::vector<TypePtr>>& seen =
			closure_discriminators_[fn_scope ? fn_scope->parent : 0];
		int matching = 0;
		for (size_t i = 0; i < seen.size(); i++)
		{
			if (seen[i].size() != param_types.size())
				continue;
			bool same = true;
			for (size_t j = 0; same && j < seen[i].size(); j++)
				if (!TypeEquals(seen[i][j], param_types[j]))
					same = false;
			if (same)
				matching++;
		}
		seen.push_back(param_types);
		entity->closure_discriminator = matching;
	}
	Scope* members = model_.CreateScope(SCOPE_CLASS, name, current_);
	model_.SetMemberScope(entity, members);
	ClassInfo& cls = unit_.classes.Create(entity);
	cls.members = members;
	cls.is_aggregate = false;
	model_.MutableInfo(entity)->class_record = &cls;
	BeginClassLayout(cls);

	// operator(): const unless the lambda is mutable, body-defined.
	TypePtr member_type = MakeFunctionType(ret, param_types, false);
	member_type = MakeFunctionCvQualifiedType(
		member_type, !lambda.mutable_specifier, false);
	TypePtr adjusted = MethodAdjustedType(cls, member_type);
	fn_scope->fn_type = member_type;

	SemNodePtr item = MakeSemNode(SN_FUNCTION_DEFINITION);
	SemNode* node = item.get();
	node->name = QualifiedScopePath(members->parent) + name +
		"::operator ()";
	node->type = adjusted;
	node->entity_scope = members;
	node->entity_name = "operator ()";
	node->is_method = true;
	node->inline_def = true;
	SemNodePtr this_param = MakeSemNode(SN_PARAMETER);
	this_param->name = "this";
	this_param->type = adjusted->parameters[0];
	this_param->entity_scope = fn_scope;
	this_param->entity_name = "this";
	node->children.push_back(std::move(this_param));
	{
		ScopeBinding this_binding;
		this_binding.kind = SB_PARAMETER;
		this_binding.name = "this";
		this_binding.type = adjusted->parameters[0];
		AddBinding(*fn_scope, this_binding);
	}
	for (size_t i = 0; i < parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = parameters[i].name;
		parameter->type = parameters[i].type;
		parameter->entity_scope = fn_scope;
		parameter->entity_name = parameters[i].name;
		node->children.push_back(std::move(parameter));
	}
	MethodContext context;
	context.fn_scope = fn_scope;
	context.fn_owner = members;
	context.fn_name = "operator ()";
	context.lexical_cls = method_.cls ? method_.cls : method_.lexical_cls;
	LambdaFrame frame;
	frame.fn_scope = fn_scope;
	frame.cls = &cls;
	frame.members = members;
	frame.by_ref_default = lambda.has_capture_default &&
		lambda.capture_default == OP_AMP;
	frame.by_copy_default = lambda.has_capture_default &&
		lambda.capture_default != OP_AMP;
	frame.this_param_type = adjusted->parameters[0];
	frame.enclosing_this = CurrentThisType();
	for (size_t i = 0; i < lambda.captures.size(); i++)
	{
		if (lambda.captures[i].kind == LC_THIS)
			frame.this_spelled = true;
		else
		{
			frame.explicit_names.push_back(
				lambda.captures[i].identifier);
			frame.explicit_copy.push_back(
				lambda.captures[i].kind == LC_COPY);
		}
	}
	lambda_frames_.push_back(frame);
	// 5.1.2p14: spelled captures are members regardless of use.
	for (size_t i = 0; i < lambda.captures.size(); i++)
	{
		LambdaFrame& open = lambda_frames_.back();
		if (lambda.captures[i].kind == LC_THIS)
		{
			if (open.enclosing_this)
				EnsureThisField(open);
			continue;
		}
		const ScopeBinding* named = UnqualifiedLookup(
			current_, lambda.captures[i].identifier, SLF_ANY);
		if (named && (named->kind == SB_VARIABLE ||
		              named->kind == SB_PARAMETER))
			EnsureCaptureField(open, *named,
			                   lambda.captures[i].kind == LC_COPY);
	}
	TypePtr deduced;
	try
	{
		deduced = BindLambdaBody(lambda, node, fn_scope, context, ret);
	}
	catch (...)
	{
		lambda_frames_.pop_back();
		throw;
	}
	LambdaFrame bound = lambda_frames_.back();
	lambda_frames_.pop_back();
	FinishClassLayout(cls, *model_.MutableInfo(entity), 0);
	model_.MutableInfo(entity)->complete = true;
	if (TypeContainsAutoPlaceholder(ret))
	{
		member_type = MakeFunctionType(deduced, param_types, false);
		member_type = MakeFunctionCvQualifiedType(
			member_type, !lambda.mutable_specifier, false);
		adjusted = MethodAdjustedType(cls, member_type);
		node->type = adjusted;
		fn_scope->fn_type = member_type;
	}
	ScopeBinding binding;
	binding.kind = SB_FUNCTION;
	binding.name = "operator ()";
	binding.type = member_type;
	ScopeBinding& added = AddBinding(*members, binding);
	if (node->unwind_no)
	{
		added.fn_unwind_no.resize(1, false);
		added.fn_unwind_no[0] = true;
	}
	unit_.deferred.push_back(std::move(item));
	info.cls = &cls;
	info.captures = bound.captures;
}
