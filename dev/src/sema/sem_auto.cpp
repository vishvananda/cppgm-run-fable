#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;
using std::string;
using std::to_string;

// PA24 statement-level deduction: the auto placeholder rules for
// variables and returns (7.1.6.4), and the range-for desugaring
// (6.5.4) built on them. Split from sem_binder.cpp.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA24 assignment boundary");
}

}  // namespace

namespace {

// The declared-type pattern with the auto placeholder (the void stand
// -in) at its base: matching against the initializer's type returns
// the deduced spelling, or null on shape mismatch (7.1.6.4p6 via the
// 14.8.2.1 deduction rules over the supported declarator forms).
TypePtr MatchAutoPattern(const TypePtr& pattern, const TypePtr& arg)
{
	if (pattern->is_auto_placeholder)
		return MakeCvQualifiedType(arg, pattern->is_const,
		                           pattern->is_volatile);
	if (pattern->kind == TK_POINTER)
	{
		TypePtr from = arg;
		if (from->kind == TK_ARRAY)
			from = MakePointerType(from->target, false, false);
		if (from->kind == TK_FUNCTION)
			from = MakePointerType(from, false, false);
		if (from->kind != TK_POINTER)
			return TypePtr();
		TypePtr inner = MatchAutoPattern(pattern->target, from->target);
		if (!inner)
			return TypePtr();
		return MakePointerType(inner,
		                       pattern->is_const || from->is_const,
		                       pattern->is_volatile || from->is_volatile);
	}
	return TypePtr();
}

}  // namespace

// The shared deduction core over an analyzed initializer or return
// value: the declared placeholder pattern deduces per 14.8.2.1.
TypePtr SemBinder::DeduceAutoDeclared(const TypePtr& type,
                                      const SemValue& value,
                                      const char* what)
{
	if (!value.type)
		throw runtime_error(string("auto ") + what + " has no type");
	if (IsReferenceType(type))
	{
		// auto& / auto&& / const auto&: the referee pattern deduces
		// against the initializer's type with its cv kept; auto&&
		// forwards an lvalue as an lvalue reference (8.3.2p6 with
		// 14.8.2.1p3).
		TypePtr referee = MatchAutoPattern(type->target, value.type);
		if (!referee)
			throw runtime_error(string("auto deduction mismatch for ") +
			                    what);
		bool rvalue = type->kind == TK_RVALUE_REFERENCE &&
			value.category != VC_LVALUE;
		return MakeReferenceType(referee, rvalue, false);
	}
	// By value: the initializer decays and drops its top cv.
	TypePtr from = value.type;
	if (from->kind == TK_ARRAY)
		from = MakePointerType(from->target, false, false);
	else if (from->kind == TK_FUNCTION)
	{
		// The reference model keeps a namespace-scope auto variable
		// deduced from a (lambda) function at the function type: its
		// storage holds the address and calls spell `addr` directly.
		if (current_->kind != SCOPE_NAMESPACE)
			from = MakePointerType(from, false, false);
	}
	else
		from = RemoveTopCv(from);
	TypePtr deduced = MatchAutoPattern(type, from);
	if (!deduced)
		throw runtime_error(string("auto deduction mismatch for ") +
		                    what);
	return deduced;
}

TypePtr SemBinder::DeduceAutoVariableType(const TypePtr& type,
                                          const AstInitializer* init,
                                          const string& name)
{
	const AstExpr* expr = 0;
	if (init && init->kind == INIT_EQ)
		expr = init->expr.get();
	else if (init && init->kind == INIT_PAREN && init->args.size() == 1)
		expr = init->args[0].get();
	if (!init || (!expr && init->kind != INIT_BRACED))
		throw runtime_error("auto variable " + name +
		                    " has no initializer");
	const AstExpr* braced = 0;
	if (init->kind == INIT_BRACED)
		braced = init->expr.get();
	else if (expr && expr->kind == EK_BRACED)
		braced = expr;
	if (braced)
	{
		// PA25 7.1.6.4p6: a braced initializer deduces
		// std::initializer_list over the elements' common type.
		if (!type->is_auto_placeholder || IsReferenceType(type))
			throw OutsideBoundary("braced auto initializer form");
		if (braced->arguments.empty())
			throw runtime_error("cannot deduce from an empty braced "
			                    "list");
		vector<SemValue> probes;
		analyzer_.AnalyzeArgumentList(braced->arguments, probes);
		TypePtr element;
		for (size_t i = 0; i < probes.size(); i++)
		{
			TypePtr item = RemoveTopCv(
				IsReferenceType(probes[i].type)
					? probes[i].type->target : probes[i].type);
			if (!element)
				element = item;
			else if (!TypeEquals(element, item))
				throw runtime_error("inconsistent braced deduction "
				                    "for " + name);
		}
		return StdInitializerListType(element);
	}
	if (!expr)
		throw OutsideBoundary("braced auto initializer");
	SemValue value = analyzer_.Analyze(*expr);
	// PA24: auto deduced from a captureless lambda takes the function
	// view - a pointer at block scope, the function type itself at
	// namespace scope (the reference model).
	if (value.type && RemoveTopCv(value.type)->kind == TK_CLASS &&
	    value.node && value.node->kind == SN_CLOSURE_INIT &&
	    type->is_auto_placeholder && !type->is_const &&
	    !type->is_volatile &&
	    !closure_object_view_.count(RemoveTopCv(value.type)->named))
	{
		const Scope* owner = 0;
		string fn_name;
		TypePtr fn_type;
		if (CapturelessClosureFunction(RemoveTopCv(value.type)->named,
		                               owner, fn_name, fn_type))
		{
			SemValue view;
			view.type = fn_type;
			view.category = VC_LVALUE;
			return DeduceAutoDeclared(type, view, name.c_str());
		}
	}
	return DeduceAutoDeclared(type, value, name.c_str());
}

void SemBinder::BindReturnStatement(const AstStmt& stmt)
{
	SemNode* item = AppendItem(SN_RETURN_STATEMENT);
	if (!stmt.expr)
	{
		// 7.1.6.4p10: a placeholder return with no operand is void.
		if (TypeContainsAutoPlaceholder(current_return_))
			current_return_ = MakeFundamentalType(FT_VOID);
		if (!IsVoidType(current_return_))
			throw runtime_error("non-void function returns no value");
		return;
	}
	TypePtr bare = RemoveTopCv(current_return_);
	SemValue value;
	if (stmt.expr->kind == EK_BRACED &&
	    !IsReferenceType(current_return_) && bare->kind == TK_CLASS)
		// 6.6.3p3: a braced-init-list return copy-list-initializes
		// the result object.
		value = analyzer_.MakeTemporaryObject(bare, stmt.expr->arguments,
		                                      false, true);
	else
		value = analyzer_.Analyze(*stmt.expr);
	if (TypeContainsAutoPlaceholder(current_return_))
	{
		// 7.1.6.4p7: the first return statement deduces the placeholder
		// return type; the enclosing definition publishes it after the
		// body completes.
		current_return_ = DeduceAutoDeclared(current_return_, value,
		                                     "return value");
		bare = RemoveTopCv(current_return_);
	}
	else if (auto_return_pattern_ && stmt.expr->kind != EK_BRACED)
	{
		// 7.1.6.4p9: every return statement must deduce the same type.
		TypePtr again = DeduceAutoDeclared(auto_return_pattern_, value,
		                                   "return value");
		if (!TypeEquals(again, current_return_))
			throw runtime_error("inconsistent auto return type "
			                    "deduction");
	}
	if (!IsReferenceType(current_return_) && bare->kind == TK_CLASS &&
	    value.type && RemoveTopCv(value.type)->kind == TK_CLASS &&
	    !value.function_set)
	{
		item->children.push_back(WrapReturnValue(std::move(value), bare));
		return;
	}
	analyzer_.CopyInitialize(value, current_return_, "return value");
	item->children.push_back(std::move(value.node));
}

// 12.8p32: a by-value class return copy-initializes the result object;
// an expression naming a non-volatile automatic object tries the move
// constructor first and falls back to the copy constructor.
SemNodePtr SemBinder::WrapReturnValue(SemValue value, const TypePtr& bare)
{
	if (value.category == VC_PRVALUE &&
	    RemoveTopCv(value.type)->named == bare->named)
	{
		// The prvalue constructs the result object directly; its own
		// temporary cleanup does not apply.
		if (value.node->kind == SN_CONSTRUCTOR_ACTION)
		{
			value.node->needs_dtor = false;
			while (value.node->children.size() > 1 &&
			       value.node->children.back()->kind ==
			           SN_DESTRUCTOR_ACTION)
				value.node->children.pop_back();
		}
		return std::move(value.node);
	}
	EnsureTypeCompleteness(bare->named);
	const ClassInfo* cls = unit_.classes.Find(bare->named);
	if (!cls)
		throw runtime_error("class record missing for return value");
	bool move_first = false;
	if (value.category == VC_LVALUE &&
	    value.node->kind == SN_ID_EXPRESSION && value.node->entity_scope &&
	    TypeEquals(RemoveTopCv(value.type), RemoveTopCv(bare)))
	{
		const Scope* home = value.node->entity_scope;
		const ScopeBinding* binding =
			FindOwnBinding(*home, value.node->entity_name);
		if (binding &&
		    (binding->kind == SB_VARIABLE ||
		     binding->kind == SB_PARAMETER) &&
		    !IsReferenceType(binding->type) &&
		    (home->kind == SCOPE_FUNCTION || home->kind == SCOPE_BLOCK))
			move_first = true;
	}
	vector<SemValue> args;
	args.push_back(std::move(value));
	int index = -1;
	bool resolved = false;
	if (move_first)
	{
		args[0].category = VC_XVALUE;
		try
		{
			index = ResolveClassConstructor(*cls, args, true,
			                                "return value");
			resolved = true;
		}
		catch (const std::exception&)
		{
			args[0].category = VC_LVALUE;
		}
	}
	if (!resolved)
		index = ResolveClassConstructor(*cls, args, true, "return value");
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < args.size(); i++)
		arg_nodes.push_back(std::move(args[i].node));
	SemNodePtr action = MakeConstructorCall(*cls, index, false, SemNodePtr(),
	                                        std::move(arg_nodes));
	action->synth_copy = true;
	action->type = bare;
	action->category = VC_PRVALUE;
	// Ownership transfers to the caller's result object: no temporary
	// cleanup attaches.
	return action;
}

// --- range-for desugaring (6.5.4) ------------------------------------------

namespace {

AstExprPtr MakeSynthId(const string& name)
{
	AstExprPtr expr(new AstExpr(EK_ID));
	AstNamePart part;
	part.kind = NP_IDENTIFIER;
	part.identifier = name;
	expr->name.parts.push_back(std::move(part));
	return expr;
}

AstExprPtr MakeSynthIntLiteral(unsigned long long value)
{
	AstExprPtr expr(new AstExpr(EK_LITERAL));
	expr->literal_kind = PTK_LITERAL;
	expr->literal_type = FT_INT;
	expr->literal = to_string(value);
	expr->literal_elements = 1;
	string data(4, '\0');
	for (size_t b = 0; b < 4; b++)
		data[b] = (char)((value >> (8 * b)) & 0xff);
	expr->literal_data = data;
	return expr;
}

AstExprPtr MakeSynthUnary(ETokenType op, const char* spelling,
                          AstExprPtr operand)
{
	AstExprPtr expr(new AstExpr(EK_UNARY));
	expr->op = op;
	expr->op_spelling = spelling;
	expr->operands.push_back(std::move(operand));
	return expr;
}

AstExprPtr MakeSynthBinary(ETokenType op, const char* spelling,
                           AstExprPtr left, AstExprPtr right)
{
	AstExprPtr expr(new AstExpr(EK_BINARY));
	expr->op = op;
	expr->op_spelling = spelling;
	expr->operands.push_back(std::move(left));
	expr->operands.push_back(std::move(right));
	return expr;
}

TypePtr MakeAutoPlaceholder()
{
	Type marked;
	marked.kind = TK_FUNDAMENTAL;
	marked.fundamental = FT_VOID;
	marked.is_auto_placeholder = true;
	return std::make_shared<Type>(marked);
}

}  // namespace

// One hidden range-for name (`__range1`, `__begin1`, `__idx2`, ...):
// the counter runs per function body in declaration order, matching
// the reference presentation.
string SemBinder::NextRangeForName(const char* stem)
{
	return string(stem) + to_string(++range_hidden_counter_);
}

// Binds a synthesized hidden or loop variable declaration through the
// ordinary variable path (auto deduction, class initialization, and
// reference binding included); the initializer AST stays owned here.
void SemBinder::BindSynthesizedVariable(const string& name,
                                        const TypePtr& type,
                                        AstExprPtr init_expr,
                                        const DeclSpecifierInfo& specs)
{
	std::unique_ptr<AstInitializer> init(new AstInitializer());
	init->kind = INIT_EQ;
	init->expr = std::move(init_expr);
	BindVariable(name, type, init.get(), specs);
	synth_inits_.push_back(std::move(init));
}

void SemBinder::BindRangeForStatement(const AstStmt& stmt)
{
	Scope* saved = current_;
	current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
	const AstExpr* range_ast = stmt.for_range_init.get();
	while (range_ast->kind == EK_PAREN)
		range_ast = range_ast->operands[0].get();
	// The declared loop name: a plain range identifier stands in for
	// the hidden __range binding only while the loop declaration does
	// not shadow it (6.5.4p1 evaluates range-init outside the loop
	// -variable scope, so `for (int a : a)` reads the enclosing `a`).
	string loop_name;
	if (!stmt.for_range_decl->declarators.empty())
		if (const AstName* loop_id =
		        stmt.for_range_decl->declarators[0].declarator->IdName())
			if (loop_id->IsPlainIdentifier())
				loop_name = loop_id->parts[0].identifier;
	string range_name;
	TypePtr range_bare;
	if (range_ast->kind == EK_BRACED)
	{
		// A braced range materializes as a hidden bounded array whose
		// element type comes from the first element.
		if (range_ast->arguments.empty())
			throw OutsideBoundary("empty braced range");
		SemValue probe = analyzer_.Analyze(*range_ast->arguments[0]);
		TypePtr element = RemoveTopCv(probe.type);
		if (element->kind == TK_ARRAY || element->kind == TK_FUNCTION)
			throw OutsideBoundary("braced range element form");
		range_name = NextRangeForName("__range");
		ScopeBinding binding;
		binding.kind = SB_VARIABLE;
		binding.name = range_name;
		binding.type = MakeArrayType(element, false, 0);
		ScopeBinding& added = AddBinding(*current_, binding);
		SemNode* item = AppendItem(SN_VARIABLE);
		item->name = range_name;
		item->entity_scope = added.owner;
		item->entity_name = range_name;
		item->has_explicit_init = true;
		TypePtr completed = added.type;
		item->children.push_back(analyzer_.AnalyzeBracedInit(
			range_ast->arguments, completed));
		added.type = completed;
		item->type = completed;
		range_bare = completed;
	}
	else
	{
		SemValue range = analyzer_.Analyze(*range_ast);
		range_bare = RemoveTopCv(range.type);
		if (range_ast->kind == EK_ID &&
		    range_ast->name.IsPlainIdentifier() &&
		    range_ast->name.parts[0].identifier != loop_name)
			range_name = range_ast->name.parts[0].identifier;
		else
		{
			// 6.5.4p1: auto&& __range = range-init (the hidden
			// reference keeps a temporary range alive for the loop).
			range_name = NextRangeForName("__range");
			ScopeBinding binding;
			binding.kind = SB_VARIABLE;
			binding.name = range_name;
			binding.type = MakeReferenceType(
				range.type, range.category != VC_LVALUE, false);
			ScopeBinding& added = AddBinding(*current_, binding);
			SemNode* item = AppendItem(SN_VARIABLE);
			item->name = range_name;
			item->type = added.type;
			item->entity_scope = added.owner;
			item->entity_name = range_name;
			item->has_explicit_init = true;
			analyzer_.CopyInitialize(range, added.type, "range binding");
			item->children.push_back(std::move(range.node));
		}
	}
	AstExprPtr loop_init;
	AstExprPtr cond_expr;
	AstExprPtr iter_expr;
	if (range_bare->kind == TK_ARRAY)
	{
		if (!range_bare->bound_known)
			throw runtime_error("range-for over an array of unknown "
			                    "bound");
		// for (int __idx = 0; __idx < bound; ++__idx)
		//     loop-decl = __range[__idx];
		string idx_name = NextRangeForName("__idx");
		DeclSpecifierInfo idx_specs;
		idx_specs.type = MakeFundamentalType(FT_INT);
		BindSynthesizedVariable(idx_name, idx_specs.type,
		                        MakeSynthIntLiteral(0), idx_specs);
		cond_expr = MakeSynthBinary(OP_LT, "<", MakeSynthId(idx_name),
		                            MakeSynthIntLiteral(range_bare->bound));
		iter_expr = MakeSynthUnary(OP_INC, "++", MakeSynthId(idx_name));
		AstExprPtr subscript(new AstExpr(EK_SUBSCRIPT));
		subscript->operands.push_back(MakeSynthId(range_name));
		subscript->operands.push_back(MakeSynthId(idx_name));
		loop_init = std::move(subscript);
	}
	else if (range_bare->kind == TK_CLASS)
	{
		// begin-expr / end-expr: members when the class declares
		// either name, argument-dependent lookup otherwise (6.5.4p1).
		EnsureTypeCompleteness(range_bare->named);
		bool member_form = false;
		for (const Scope* link = model_.MemberScope(range_bare->named);
		     link; link = link->class_base)
			if (FindOwnBinding(*link, "begin") ||
			    FindOwnBinding(*link, "end"))
			{
				member_form = true;
				break;
			}
		if (!member_form && IsStdInitializerList(range_bare, 0))
		{
			// PA25: the builtin record iterates by index over its
			// {__begin_, __size_} fields:
			// for (int __idx = 0; __idx < r.__size_; ++__idx)
			//     loop-decl = r.__begin_[__idx];
			string idx_name = NextRangeForName("__idx");
			DeclSpecifierInfo idx_specs;
			idx_specs.type = MakeFundamentalType(FT_INT);
			BindSynthesizedVariable(idx_name, idx_specs.type,
			                        MakeSynthIntLiteral(0), idx_specs);
			AstExprPtr size_member(new AstExpr(EK_MEMBER));
			size_member->op = OP_DOT;
			size_member->op_spelling = ".";
			size_member->operands.push_back(MakeSynthId(range_name));
			AstNamePart size_part;
			size_part.kind = NP_IDENTIFIER;
			size_part.identifier = "__size_";
			size_member->name.parts.push_back(std::move(size_part));
			cond_expr = MakeSynthBinary(OP_LT, "<",
			                            MakeSynthId(idx_name),
			                            std::move(size_member));
			iter_expr = MakeSynthUnary(OP_INC, "++",
			                           MakeSynthId(idx_name));
			AstExprPtr begin_member(new AstExpr(EK_MEMBER));
			begin_member->op = OP_DOT;
			begin_member->op_spelling = ".";
			begin_member->operands.push_back(MakeSynthId(range_name));
			AstNamePart begin_part;
			begin_part.kind = NP_IDENTIFIER;
			begin_part.identifier = "__begin_";
			begin_member->name.parts.push_back(std::move(begin_part));
			AstExprPtr subscript(new AstExpr(EK_SUBSCRIPT));
			subscript->operands.push_back(std::move(begin_member));
			subscript->operands.push_back(MakeSynthId(idx_name));
			loop_init = std::move(subscript);
			goto desugared;
		}
		string begin_name = NextRangeForName("__begin");
		string end_name = NextRangeForName("__end");
		const char* fns[2] = {"begin", "end"};
		const string* names[2] = {&begin_name, &end_name};
		for (int i = 0; i < 2; i++)
		{
			AstExprPtr call(new AstExpr(EK_CALL));
			if (member_form)
			{
				AstExprPtr member(new AstExpr(EK_MEMBER));
				member->op = OP_DOT;
				member->op_spelling = ".";
				member->operands.push_back(MakeSynthId(range_name));
				AstNamePart part;
				part.kind = NP_IDENTIFIER;
				part.identifier = fns[i];
				member->name.parts.push_back(std::move(part));
				call->operands.push_back(std::move(member));
			}
			else
			{
				call->operands.push_back(MakeSynthId(fns[i]));
				call->arguments.push_back(MakeSynthId(range_name));
			}
			DeclSpecifierInfo auto_specs;
			auto_specs.is_auto = true;
			auto_specs.type = MakeAutoPlaceholder();
			BindSynthesizedVariable(*names[i], auto_specs.type,
			                        std::move(call), auto_specs);
		}
		cond_expr = MakeSynthBinary(OP_NE, "!=", MakeSynthId(begin_name),
		                            MakeSynthId(end_name));
		iter_expr = MakeSynthUnary(OP_INC, "++",
		                           MakeSynthId(begin_name));
		loop_init = MakeSynthUnary(OP_STAR, "*",
		                           MakeSynthId(begin_name));
	}
	else
		throw OutsideBoundary("range-for initializer form");

desugared:
	SemNode* item = AppendItem(SN_FOR_STATEMENT);
	parents_.push_back(item);
	{
		SemNode* cond = AppendItem(SN_CONDITION);
		parents_.push_back(cond);
		SemValue value = analyzer_.Analyze(*cond_expr);
		analyzer_.RequireContextualBool(value, "condition");
		cond->children.push_back(std::move(value.node));
		parents_.pop_back();
	}
	{
		SemNode* iteration = AppendItem(SN_ITERATION);
		SemValue value = analyzer_.Analyze(*iter_expr);
		iteration->children.push_back(std::move(value.node));
	}
	synth_exprs_.push_back(std::move(cond_expr));
	synth_exprs_.push_back(std::move(iter_expr));
	// The loop body: the for-range-declaration initialized from the
	// current element, then the user statement, in a per-iteration
	// scope.
	SemNode* body = AppendItem(SN_COMPOUND_STATEMENT);
	parents_.push_back(body);
	Scope* body_saved = current_;
	current_ = model_.CreateScope(SCOPE_BLOCK, "", body_saved);
	{
		const AstDecl& decl = *stmt.for_range_decl;
		DeclSpecifierInfo specs =
			builder_.ProcessSpecifiers(decl.specifiers, true);
		const AstInitDeclarator& declarator = decl.declarators[0];
		DeclaratorInfo composed = builder_.ComposeDeclarator(
			declarator.declarator.get(), specs.type);
		if (!composed.id || !composed.id->IsPlainIdentifier())
			throw OutsideBoundary("range declaration form");
		current_declarator_begin_token_ = declarator.begin_token;
		BindSynthesizedVariable(composed.id->parts[0].identifier,
		                        composed.type, std::move(loop_init),
		                        specs);
	}
	BindStatement(*stmt.body);
	current_ = body_saved;
	parents_.pop_back();
	parents_.pop_back();
	current_ = saved;
}

