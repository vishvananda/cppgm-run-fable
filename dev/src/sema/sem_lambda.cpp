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
	for (size_t i = lambda_.frames.size(); i > 0; i--)
		if (lambda_.frames[i - 1].fn_scope == method_.fn_scope)
			return &lambda_.frames[i - 1];
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
	field.captured_this = true;
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
		member->captured_this = true;
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
	// 5.1.2p16: without `mutable`, operator() is const, so by-copy
	// members read as const lvalues.
	TypePtr read = capture.referee;
	if (capture.by_copy && !frame->is_mutable)
		read = MakeCvQualifiedType(read, true, false);
	out = SemValue();
	out.type = read;
	out.category = VC_LVALUE;
	out.node = MakeSemNode(SN_MEMBER_EXPRESSION);
	out.node->name = capture.name;
	out.node->type = read;
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
	std::map<const NamedTypeInfo*, SemClosureFunction>::const_iterator
		found = lambda_.closure_functions.find(cls);
	if (found == lambda_.closure_functions.end())
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
	// PA34: a templated lambda binds only through its immediate
	// invocation (the head's argument aliases are then in scope).
	if (lambda.template_head && lambda_.invoked_templated != &lambda)
		throw OutsideBoundary("uninvoked templated lambda");
	// One synthesis per (lambda, enclosing body): the deduction and
	// initialization analyses of one declaration share it, while a
	// re-instantiated template body synthesizes its own.
	std::pair<const void*, const void*> key(&lambda, method_.fn_scope);
	std::map<std::pair<const void*, const void*>,
	         LambdaInfo>::iterator found = lambda_.cache.find(key);
	if (found != lambda_.cache.end())
		return MakeLambdaValue(found->second);
	for (size_t i = 0; i < lambda.captures.size(); i++)
		if (lambda.captures[i].pack)
			throw OutsideBoundary("pack capture");
	bool captureless = !lambda.has_capture_default &&
		lambda.captures.empty();

	++lambda_.counter;
	string name = "__lambda" + to_string(lambda_.counter);
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
	LambdaInfo& cached = lambda_.cache[key];
	cached = info;
	return MakeLambdaValue(cached);
}

// PA34 hosted C++20 templated lambdas. The checked-in uses immediately
// invoke the lambda, so the head's parameters deduce from the call
// arguments (14.8.2.1 over the parameter clause) and the closure binds
// with the deduced argument aliases in scope; an uninvoked templated
// lambda stays a documented boundary.
SemValue SemBinder::AnalyzeTemplatedLambdaInvoke(const AstExpr& expr,
                                                 const vector<SemValue>& args)
{
	const AstLambda& lambda = *expr.lambda;
	TemplateInfo shadow;
	CollectTemplateParams(*lambda.template_head, shadow.params);
	shadow.declaring = current_;
	// Per-parameter deduction patterns: the head's names bind the
	// positional placeholders while each declared type composes.
	vector<TypePtr> patterns;
	vector<bool> pattern_packs;
	{
		Scope* pattern_scope =
			MakePatternParamScope(shadow.params, current_);
		Scope* saved = current_;
		Scope* saved_capture = param_capture_scope_;
		current_ = pattern_scope;
		param_capture_scope_ = 0;
		const AstParameterClause* clause =
			lambda.has_declarator ? lambda.parameters.get() : 0;
		for (size_t i = 0; clause && i < clause->parameters.size(); i++)
		{
			const AstParameter& parameter = clause->parameters[i];
			bool is_pack = false;
			if (parameter.declarator)
				for (size_t d = 0;
				     d < parameter.declarator->items.size(); d++)
					if (parameter.declarator->items[d].kind == DI_PACK)
						is_pack = true;
			TypePtr pattern;
			try
			{
				DeclSpecifierInfo specs = builder_.ProcessSpecifiers(
					parameter.specifiers, false);
				DeclaratorInfo composed = builder_.ComposeDeclarator(
					parameter.declarator.get(), specs.type);
				pattern = composed.type;
			}
			catch (const std::exception&)
			{
				pattern = TypePtr();  // non-deduced context
			}
			patterns.push_back(pattern);
			pattern_packs.push_back(is_pack);
		}
		current_ = saved;
		param_capture_scope_ = saved_capture;
	}
	size_t pack_index = TemplatePackIndex(shadow.params);
	bool has_pack = pack_index < shadow.params.size();
	vector<TemplateArg> bound(shadow.params.size());
	for (size_t i = 0; i < bound.size(); i++)
		bound[i].is_pack_slot = shadow.params[i].pack;
	vector<TemplateArg> pack_elements;
	size_t p = 0;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (p >= patterns.size())
			throw runtime_error("templated lambda call has too many "
			                    "arguments");
		if (pattern_packs[p])
			throw OutsideBoundary("pack-expanded templated-lambda "
			                      "parameter");
		const TypePtr pattern = patterns[p++];
		// A failed unification leaves the parameter to the closure
		// call's ordinary conversion check.
		if (pattern)
			DeduceFixedParameter(pattern, args[i], bound);
	}
	if (has_pack && !bound[pack_index].pack_done)
	{
		// An unmentioned head pack deduces the empty run.
		bound[pack_index].pack_done = true;
		bound[pack_index].pack_elements = pack_elements;
	}
	if (!FillDeducedDefaults(shadow, bound, pack_elements))
		throw runtime_error("templated lambda argument deduction "
		                    "failed");
	for (size_t i = 0; i < bound.size(); i++)
		if (!ArgBound(bound[i]))
			throw runtime_error("templated lambda argument deduction "
			                    "failed");
	Scope* alias_scope = MakeArgumentAliasScope(shadow, bound);
	Scope* saved = current_;
	const AstLambda* saved_invoked = lambda_.invoked_templated;
	current_ = alias_scope;
	lambda_.invoked_templated = &lambda;
	SemValue value;
	try
	{
		value = AnalyzeLambda(expr);
	}
	catch (...)
	{
		current_ = saved;
		lambda_.invoked_templated = saved_invoked;
		throw;
	}
	current_ = saved;
	lambda_.invoked_templated = saved_invoked;
	return value;
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
	lambda_.frames.push_back(frame);
	TypePtr deduced;
	try
	{
		deduced = BindLambdaBody(lambda, node, fn_scope, context, ret);
	}
	catch (...)
	{
		lambda_.frames.pop_back();
		throw;
	}
	lambda_.frames.pop_back();
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
	// 8.3.6 (PA34): lambda parameter default arguments follow the
	// ordinary omitted-trailing-argument path.
	added.fn_defaults.resize(1);
	for (size_t i = 0; i < parameters.size(); i++)
		added.fn_defaults[0].push_back(parameters[i].default_arg);
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
	lambda_.closure_functions[info.cls->entity].owner = model_.global();
	lambda_.closure_functions[info.cls->entity].name = name;
	lambda_.closure_functions[info.cls->entity].type = fn_type;
	if (StmtDeclaresClass(lambda.body.get()))
		lambda_.closure_object_view.insert(info.cls->entity);
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
	// The operator() body scope belongs to this closure: a nested
	// lambda's local encoding recurses through it (5.1.7).
	fn_scope->closure_entity = entity;
	{
		// 5.1.7: the <lambda-sig> discriminator counts earlier
		// lambdas with the same signature in the same context: the
		// enclosing function body (the mangled local-name prefix,
		// lower_name.cpp LocalEntityFunctionScope), not the immediate
		// block.
		const Scope* context = fn_scope ? fn_scope->parent : 0;
		for (const Scope* scope = context; scope && scope->parent;
		     scope = scope->parent)
		{
			if (scope->kind == SCOPE_FUNCTION)
			{
				context = scope;
				break;
			}
			if (scope->kind == SCOPE_NAMESPACE)
				break;
		}
		std::vector<std::vector<TypePtr>>& seen =
			lambda_.closure_discriminators[context];
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
	frame.is_mutable = lambda.mutable_specifier;
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
	lambda_.frames.push_back(frame);
	// 5.1.2p14: spelled captures are members regardless of use.
	for (size_t i = 0; i < lambda.captures.size(); i++)
	{
		LambdaFrame& open = lambda_.frames.back();
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
		lambda_.frames.pop_back();
		throw;
	}
	LambdaFrame bound = lambda_.frames.back();
	lambda_.frames.pop_back();
	FinishClassLayout(cls, *model_.MutableInfo(entity), 0);
	model_.MutableInfo(entity)->complete = true;
	DeclareClosureSpecialMembers(cls);
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
	// 8.3.6 (PA34): lambda parameter default arguments follow the
	// ordinary omitted-trailing-argument path.
	added.fn_defaults.resize(1);
	for (size_t i = 0; i < parameters.size(); i++)
		added.fn_defaults[0].push_back(parameters[i].default_arg);
	unit_.deferred.push_back(std::move(item));
	info.cls = &cls;
	info.captures = bound.captures;
}

// PA29 GNU statement expression: the compound statement binds into a
// detached holder in the current function context (block scope and
// all); the last expression statement's value is the expression's
// value, a prvalue whose temporary lives to the end of the enclosing
// full-expression.
SemValue SemBinder::AnalyzeStatementExpression(const AstExpr& expr)
{
	SemNodePtr holder = MakeSemNode(SN_STATEMENT_EXPRESSION);
	parents_.push_back(holder.get());
	BindStatement(*expr.stmt_body);
	parents_.pop_back();
	SemValue value;
	value.type = MakeFundamentalType(FT_VOID);
	const SemNode* compound = holder->children.empty()
		? 0 : holder->children.back().get();
	const SemNode* last = compound && !compound->children.empty()
		? compound->children.back().get() : 0;
	if (last && last->kind == SN_EXPRESSION_STATEMENT &&
	    !last->children.empty() && last->children[0]->type)
		value.type = last->children[0]->type;
	value.category = VC_PRVALUE;
	holder->type = value.type;
	holder->category = VC_PRVALUE;
	value.node = std::move(holder);
	return value;
}
