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

SemNodePtr SemBinder::ThisValueNode()
{
	LambdaFrame* frame = ActiveLambdaFrame();
	if (frame && frame->cls && frame->enclosing_this)
	{
		// 5.1.2p17-adjacent: `this` inside the lambda body reads the
		// captured pointer field.
		if (!frame->this_captured)
		{
			if (!frame->by_ref_default && !frame->this_spelled)
				throw runtime_error("this is not captured");
			ClassField field;
			field.name = "__this";
			field.type = frame->enclosing_this;
			field.access = MA_PUBLIC;
			ClassField& placed = LayoutField(*frame->cls, field);
			frame->this_captured = true;
			frame->this_offset = placed.offset;
			LambdaCapture capture;
			capture.is_this = true;
			frame->captures.push_back(capture);
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
	for (size_t i = 0; i < frame->explicit_names.size(); i++)
		if (frame->explicit_names[i] == binding.name)
			spelled = true;
	if (!frame->by_ref_default && !spelled)
		throw runtime_error(binding.name + " is not captured");
	size_t index = frame->captures.size();
	for (size_t i = 0; i < frame->captures.size(); i++)
		if (frame->captures[i].binding == &binding)
			index = i;
	if (index == frame->captures.size())
	{
		// First use: a by-reference capture appends a reference field
		// (the closure stores the entity's address).
		TypePtr referee = binding.type;
		if (IsReferenceType(referee))
			referee = referee->target;
		ClassField field;
		field.name = binding.name;
		field.type = MakeReferenceType(referee, false, false);
		field.access = MA_PUBLIC;
		ClassField& placed = LayoutField(*frame->cls, field);
		LambdaCapture capture;
		capture.binding = &binding;
		capture.name = binding.name;
		capture.offset = placed.offset;
		capture.referee = referee;
		frame->captures.push_back(capture);
	}
	const LambdaCapture& capture = frame->captures[index];
	out = SemValue();
	out.type = capture.referee;
	out.category = VC_LVALUE;
	out.node = MakeSemNode(SN_MEMBER_EXPRESSION);
	out.node->name = capture.name;
	out.node->type = capture.referee;
	out.node->member_ref = true;
	out.node->category = VC_LVALUE;
	out.node->member_offset = capture.offset;
	out.node->children.push_back(ClosureThisId(*frame));
	return true;
}

// Rebuilds the expression value of an already-synthesized lambda: the
// function name for a captureless one, a closure-construction value
// (capture sources re-analyzed in the current context) otherwise.
SemValue SemBinder::MakeLambdaValue(const LambdaInfo& info)
{
	SemValue value;
	if (info.captureless)
	{
		const ScopeBinding* binding =
			FindOwnBinding(*model_.global(), info.fn_name);
		if (!binding)
			throw runtime_error("lambda function binding missing");
		value.function_set = true;
		value.overloads.push_back(info.fn_type);
		value.overload_specs.resize(1, 0);
		value.fn_owner = binding->owner;
		value.fn_name = info.fn_name;
		value.category = VC_LVALUE;
		value.type = info.fn_type;
		value.member_type = info.fn_type;
		value.node = MakeSemNode(SN_ID_EXPRESSION);
		value.node->name = info.fn_name;
		value.node->type = info.fn_type;
		value.node->category = VC_LVALUE;
		value.node->entity_scope = binding->owner;
		value.node->entity_name = info.fn_name;
		return value;
	}
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
	if (lambda.mutable_specifier)
		throw OutsideBoundary("mutable lambda");
	if (lambda.has_capture_default && lambda.capture_default != OP_AMP)
		throw OutsideBoundary("copy capture default");
	for (size_t i = 0; i < lambda.captures.size(); i++)
	{
		if (lambda.captures[i].kind == LC_COPY)
			throw OutsideBoundary("copy capture");
		if (lambda.captures[i].pack)
			throw OutsideBoundary("pack capture");
	}
	bool captureless = !lambda.has_capture_default &&
		lambda.captures.empty();

	string name = "__lambda" + to_string(++lambda_counter_);
	if (!method_.fn_name.empty())
	{
		string stem;
		for (size_t i = 0; i < method_.fn_name.size(); i++)
		{
			char c = method_.fn_name[i];
			bool word = (c >= 'a' && c <= 'z') ||
				(c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
				c == '_';
			stem += word ? c : '_';
		}
		name = "__lambda_" + stem + "_" + to_string(lambda_counter_);
	}

	// The operator's scope chains to the surrounding context so
	// enclosing types, template parameters, and globals resolve.
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, name, current_);
	vector<ParameterInfo> parameters;
	vector<TypePtr> param_types;
	if (lambda.has_declarator && lambda.parameters)
		builder_.BuildParameters(*lambda.parameters, parameters,
		                         param_types, 0);
	for (size_t i = 0; i < parameters.size(); i++)
	{
		if (parameters[i].name.empty())
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
	Scope* members = model_.CreateScope(SCOPE_CLASS, name, current_);
	model_.SetMemberScope(entity, members);
	ClassInfo& cls = unit_.classes.Create(entity);
	cls.members = members;
	cls.is_aggregate = false;
	model_.MutableInfo(entity)->class_record = &cls;
	BeginClassLayout(cls);

	// operator(): const (non-mutable lambda), body-defined.
	TypePtr member_type = MakeFunctionType(ret, param_types, false);
	member_type = MakeFunctionCvQualifiedType(member_type, true, false);
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
	frame.by_ref_default = lambda.has_capture_default;
	frame.this_param_type = adjusted->parameters[0];
	frame.enclosing_this = CurrentThisType();
	for (size_t i = 0; i < lambda.captures.size(); i++)
	{
		if (lambda.captures[i].kind == LC_THIS)
			frame.this_spelled = true;
		else
			frame.explicit_names.push_back(
				lambda.captures[i].identifier);
	}
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
	LambdaFrame bound = lambda_frames_.back();
	lambda_frames_.pop_back();
	FinishClassLayout(cls, *model_.MutableInfo(entity), 0);
	model_.MutableInfo(entity)->complete = true;
	if (TypeContainsAutoPlaceholder(ret))
	{
		member_type = MakeFunctionType(deduced, param_types, false);
		member_type = MakeFunctionCvQualifiedType(member_type, true,
		                                          false);
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
