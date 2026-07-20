#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// The statement side of the semantic binder (split from
// sem_binder.cpp): the shared BindStatement dispatch and the per-form
// binders, including the PA34 hosted C++17 selection-statement forms
// (init-statements and constexpr-if).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA12 assignment boundary");
}

}  // namespace

// --- statements ---------------------------------------------------------------

void SemBinder::BindStatement(const AstStmt& stmt)
{
	switch (stmt.kind)
	{
	case SK_COMPOUND:
		BindCompoundStatement(stmt);
		return;
	case SK_DECLARATION:
		BindDeclarationStatement(stmt);
		return;
	case SK_EXPRESSION:
		BindExpressionStatement(stmt);
		return;
	case SK_RETURN:
		BindReturnStatement(stmt);
		return;
	case SK_IF:
		BindIfStatement(stmt);
		return;
	case SK_WHILE:
		BindWhileStatement(stmt);
		return;
	case SK_DO:
		BindDoStatement(stmt);
		return;
	case SK_FOR:
		BindForStatement(stmt);
		return;
	case SK_SWITCH:
		BindSwitchStatement(stmt);
		return;
	case SK_CASE:
	case SK_DEFAULT:
		BindLabelStatement(stmt);
		return;
	case SK_THROW:
		BindThrowStatement(stmt);
		return;
	case SK_TRY:
		BindTryStatement(stmt);
		return;
	case SK_BREAK:
		AppendItem(SN_BREAK_STATEMENT);
		return;
	case SK_CONTINUE:
		AppendItem(SN_CONTINUE_STATEMENT);
		return;
	case SK_GOTO:
		AppendItem(SN_GOTO_STATEMENT)->name = stmt.label;
		return;
	case SK_LABELED:
	{
		SemNode* item = AppendItem(SN_LABEL_STATEMENT);
		item->name = stmt.label;
		parents_.push_back(item);
		BindStatement(*stmt.body);
		parents_.pop_back();
		return;
	}
	default:
		throw OutsideBoundary("statement form");
	}
}

void SemBinder::BindCompoundStatement(const AstStmt& stmt)
{
	SemNode* item = AppendItem(SN_COMPOUND_STATEMENT);
	parents_.push_back(item);
	Scope* saved = current_;
	current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
	for (size_t i = 0; i < stmt.items.size(); i++)
		BindStatement(*stmt.items[i]);
	current_ = saved;
	parents_.pop_back();
}

void SemBinder::BindDeclarationStatement(const AstStmt& stmt)
{
	const AstDecl& decl = *stmt.decl;
	switch (decl.kind)
	{
	case DK_ALIAS:           // prints its own type-alias line
	case DK_USING_DECLARATION:
	case DK_USING_DIRECTIVE:
	case DK_NAMESPACE_ALIAS:
	case DK_EMPTY:
		BindDeclaration(decl);
		return;
	default:
		break;
	}
	if (TryVexingCallRecovery(decl))
		return;
	SemNode* item = AppendItem(SN_SIMPLE_DECLARATION);
	parents_.push_back(item);
	BindDeclaration(decl);
	parents_.pop_back();
}

void SemBinder::BindExpressionStatement(const AstStmt& stmt)
{
	if (!stmt.expr)
		return;  // the empty statement dumps nothing
	SemNode* item = AppendItem(SN_EXPRESSION_STATEMENT);
	SemValue value = analyzer_.Analyze(*stmt.expr);
	item->children.push_back(std::move(value.node));
}

// PA25 15.1: a throw statement is an expression statement over the
// throw-expression.
void SemBinder::BindThrowStatement(const AstStmt& stmt)
{
	SemNode* item = AppendItem(SN_EXPRESSION_STATEMENT);
	SemValue value = analyzer_.AnalyzeThrow(stmt.expr.get());
	item->children.push_back(std::move(value.node));
}

// PA25 15: a try block with its handlers. Each handler binds its
// exception declaration in a fresh block scope around the handler
// body.
void SemBinder::BindTryStatement(const AstStmt& stmt,
                                 const DeferredBody* ctor_inits,
                                 const ClassInfo* dtor_epilogue_cls)
{
	SemNode* item = AppendItem(SN_TRY);
	parents_.push_back(item);
	if (ctor_inits)
	{
		// PA29 constructor function-try-block: the member and base
		// initialization actions run inside the try region, and the
		// handlers implicitly rethrow (15.3p15).
		item->function_try = true;
		AnalyzeMemberInits(*ctor_inits, *item);
	}
	BindStatement(*stmt.body);
	if (dtor_epilogue_cls)
	{
		// PA29 destructor function-try-block: the member and base
		// destructions run inside the try region (before any handler
		// is entered, 15.2p11), and the handlers implicitly rethrow
		// (15.3p15).
		item->function_try = true;
		AnalyzeDtorEpilogue(*dtor_epilogue_cls, *item);
	}
	for (size_t i = 0; i < stmt.handlers.size(); i++)
	{
		const AstHandler& handler = stmt.handlers[i];
		SemNode* handler_item = AppendItem(SN_CATCH_HANDLER);
		parents_.push_back(handler_item);
		Scope* saved = current_;
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
		if (!handler.ellipsis)
		{
			DeclSpecifierInfo specs =
				builder_.ProcessSpecifiers(handler.specifiers, true);
			TypePtr type = specs.type;
			string var_name;
			if (handler.declarator)
			{
				DeclaratorInfo composed = builder_.ComposeDeclarator(
					handler.declarator.get(), specs.type);
				type = composed.type;
				if (composed.id && composed.id->IsPlainIdentifier())
					var_name = composed.id->parts[0].identifier;
			}
			handler_item->type = type;
			TypePtr bare = RemoveTopCv(
				IsReferenceType(type) ? type->target : type);
			if (bare->kind == TK_CLASS)
				RequireCompleteType(bare->named);
			else if (bare->kind != TK_FUNDAMENTAL)
				throw OutsideBoundary("handler type form");
			if (bare->kind == TK_CLASS && !IsReferenceType(type))
			{
				// 15.3p16-p17: a by-value handler copy-initializes its
				// parameter object from the exception object and
				// destroys it when the handler exits; the resolved
				// calls pin on the handler node for the lowering.
				const ClassInfo* cls = unit_.classes.Find(bare->named);
				if (!cls)
					throw runtime_error("handler class record missing");
				vector<SemValue> args;
				SemValue source;
				source.type = MakeCvQualifiedType(bare, true, false);
				source.category = VC_LVALUE;
				source.node = MakeSemNode(SN_LITERAL);
				source.node->type = source.type;
				args.push_back(std::move(source));
				int index = ResolveClassCtorHost(*cls, args, true,
				                                 "catch");
				vector<SemNodePtr> arg_nodes;
				for (size_t a = 0; a < args.size(); a++)
					arg_nodes.push_back(std::move(args[a].node));
				handler_item->handler_ctor = MakeConstructorCall(
					*cls, index, false, SemNodePtr(),
					std::move(arg_nodes));
				if (unit_.classes.NeedsDestruction(*cls))
					handler_item->subobject_dtor =
						MakeDestructorCall(*cls, false, SemNodePtr());
			}
			if (!var_name.empty())
			{
				ScopeBinding binding;
				binding.kind = SB_VARIABLE;
				binding.name = var_name;
				binding.type = type;
				binding.home = current_;
				AddBinding(*current_, binding);
				handler_item->name = var_name;
				handler_item->entity_scope = current_;
				handler_item->entity_name = var_name;
			}
		}
		BindStatement(*handler.body);
		current_ = saved;
		parents_.pop_back();
	}
	parents_.pop_back();
}

void SemBinder::BindConditionDeclaration(const AstCondition& condition,
                                         bool for_switch)
{
	SemNode* item = AppendItem(SN_CONDITION_DECLARATION);
	parents_.push_back(item);
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(condition.specifiers, true);
	DeclaratorInfo composed =
		builder_.ComposeDeclarator(condition.declarator.get(), specs.type);
	if (!composed.id || !composed.id->IsPlainIdentifier())
		throw OutsideBoundary("condition declarator");
	if (!condition.init)
		throw runtime_error("condition declaration requires an "
		                    "initializer");
	TypePtr type = composed.type;
	if (specs.is_constexpr)
		type = MakeCvQualifiedType(type, true, false);
	const string& var_name = composed.id->parts[0].identifier;
	BindVariable(var_name, type, condition.init.get(), specs);

	// 6.4p4: the condition value is the (converted) declared variable.
	SemValue value;
	value.type = IsReferenceType(type) ? type->target : type;
	value.category = VC_LVALUE;
	TypePtr bare = RemoveTopCv(value.type);
	if (bare->kind == TK_CLASS)
	{
		// A class condition variable converts through its conversion
		// function; the call attaches as the condition value child.
		const ScopeBinding* binding = FindOwnBinding(*current_, var_name);
		ScopeBinding fallback;
		if (!binding)
		{
			fallback.name = var_name;
			fallback.type = type;
			fallback.home = current_;
			binding = &fallback;
		}
		ScopeBinding local = *binding;
		local.home = local.home ? local.home : current_;
		SemValue object;
		object.node = MakeSemNode(SN_ID_EXPRESSION);
		object.node->name = var_name;
		object.node->type = value.type;
		object.node->category = VC_LVALUE;
		object.node->entity_scope =
			binding->owner ? binding->owner : current_;
		object.node->entity_name = var_name;
		object.type = value.type;
		object.category = VC_LVALUE;
		if (for_switch)
		{
			if (!analyzer_.ConvertClassOperand(object) ||
			    (!IsIntegralType(object.type) &&
			     object.type->kind != TK_ENUM))
				throw runtime_error("switch condition is not integral");
		}
		else
			analyzer_.RequireContextualBool(object, "condition");
		item->children.push_back(std::move(object.node));
		parents_.pop_back();
		return;
	}
	parents_.pop_back();
	if (for_switch)
	{
		if (!IsIntegralType(value.type) && value.type->kind != TK_ENUM)
			throw runtime_error("switch condition is not integral");
	}
	else
		analyzer_.RequireContextualBool(value, "condition");
}

void SemBinder::BindCondition(const AstCondition& condition, bool for_switch)
{
	SemNode* item = AppendItem(SN_CONDITION);
	parents_.push_back(item);
	if (condition.is_declaration)
		BindConditionDeclaration(condition, for_switch);
	else
	{
		SemValue value = analyzer_.Analyze(*condition.expr);
		if (for_switch)
		{
			// 6.4.2p2: integral or (possibly scoped) enumeration type.
			if (!IsIntegralType(value.type) &&
			    value.type->kind != TK_ENUM)
				throw runtime_error("switch condition is not integral");
		}
		else
			analyzer_.RequireContextualBool(value, "condition");
		item->children.push_back(std::move(value.node));
	}
	parents_.pop_back();
}

void SemBinder::BindIfStatement(const AstStmt& stmt)
{
	Scope* saved = current_;
	// C++17 init-statement: the declared names live in an implicit
	// block around the whole selection statement; a wrapping compound
	// node reuses the ordinary declaration and lowering paths.
	SemNode* wrapper = 0;
	if (stmt.for_init)
	{
		wrapper = AppendItem(SN_COMPOUND_STATEMENT);
		parents_.push_back(wrapper);
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
		BindStatement(*stmt.for_init);
	}
	if (stmt.constexpr_if)
		BindConstexprIfStatement(stmt);
	else
	{
		SemNode* item = AppendItem(SN_IF_STATEMENT);
		parents_.push_back(item);
		Scope* before = current_;
		if (stmt.condition->is_declaration)
			current_ = model_.CreateScope(SCOPE_BLOCK, "", before);
		BindCondition(*stmt.condition, false);
		SemNode* then_item = AppendItem(SN_THEN);
		parents_.push_back(then_item);
		BindStatement(*stmt.then_branch);
		parents_.pop_back();
		if (stmt.else_branch)
		{
			SemNode* else_item = AppendItem(SN_ELSE);
			parents_.push_back(else_item);
			BindStatement(*stmt.else_branch);
			parents_.pop_back();
		}
		current_ = before;
		parents_.pop_back();
	}
	if (wrapper)
		parents_.pop_back();
	current_ = saved;
}

// C++17 6.4.1 hosted concession: the constexpr-if condition is a
// constant expression contextually converted to bool, and only the
// taken branch is analyzed, so a discarded dependent statement is
// never instantiated.
void SemBinder::BindConstexprIfStatement(const AstStmt& stmt)
{
	if (stmt.condition->is_declaration)
		throw OutsideBoundary("constexpr if condition declaration");
	ConstValue cond;
	if (!TryEvaluateConstant(*stmt.condition->expr, cond) &&
	    !TryFullConstant(*stmt.condition->expr, cond))
		throw runtime_error("constexpr if condition is not a constant "
		                    "expression");
	const AstStmt* taken = ConstValueIsNonZero(cond)
		? stmt.then_branch.get() : stmt.else_branch.get();
	const AstStmt* discarded = ConstValueIsNonZero(cond)
		? stmt.else_branch.get() : stmt.then_branch.get();
	Scope* saved = current_;
	if (taken)
	{
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
		BindStatement(*taken);
		current_ = saved;
	}
	// 6.4.1: only during the instantiation of a templated entity is
	// the discarded substatement skipped; elsewhere it is still a
	// fully checked (never executed) statement. The check binds into
	// a detached holder, outside return-type deduction (discarded
	// return statements do not participate).
	if (discarded && !instantiating_)
	{
		SemNodePtr holder = MakeSemNode(SN_COMPOUND_STATEMENT);
		parents_.push_back(holder.get());
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
		TypePtr saved_return = current_return_;
		try
		{
			BindStatement(*discarded);
		}
		catch (...)
		{
			current_return_ = saved_return;
			current_ = saved;
			parents_.pop_back();
			throw;
		}
		current_return_ = saved_return;
		current_ = saved;
		parents_.pop_back();
	}
}

void SemBinder::BindWhileStatement(const AstStmt& stmt)
{
	SemNode* item = AppendItem(SN_WHILE_STATEMENT);
	parents_.push_back(item);
	Scope* saved = current_;
	if (stmt.condition->is_declaration)
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
	BindCondition(*stmt.condition, false);
	BindStatement(*stmt.body);
	current_ = saved;
	parents_.pop_back();
}

void SemBinder::BindDoStatement(const AstStmt& stmt)
{
	SemNode* item = AppendItem(SN_DO_STATEMENT);
	parents_.push_back(item);
	BindStatement(*stmt.body);
	BindCondition(*stmt.condition, false);
	parents_.pop_back();
}

void SemBinder::BindForStatement(const AstStmt& stmt)
{
	if (stmt.for_range_decl)
	{
		BindRangeForStatement(stmt);
		return;
	}
	SemNode* item = AppendItem(SN_FOR_STATEMENT);
	parents_.push_back(item);
	Scope* saved = current_;
	current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
	if (stmt.for_init)
	{
		SemNode* init_item = AppendItem(SN_FOR_INIT);
		parents_.push_back(init_item);
		BindStatement(*stmt.for_init);
		parents_.pop_back();
	}
	if (stmt.condition)
		BindCondition(*stmt.condition, false);
	if (stmt.iteration)
	{
		SemNode* iteration = AppendItem(SN_ITERATION);
		SemValue value = analyzer_.Analyze(*stmt.iteration);
		iteration->children.push_back(std::move(value.node));
	}
	BindStatement(*stmt.body);
	current_ = saved;
	parents_.pop_back();
}

void SemBinder::BindSwitchStatement(const AstStmt& stmt)
{
	Scope* saved = current_;
	SemNode* wrapper = 0;
	if (stmt.for_init)
	{
		wrapper = AppendItem(SN_COMPOUND_STATEMENT);
		parents_.push_back(wrapper);
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
		BindStatement(*stmt.for_init);
	}
	SemNode* item = AppendItem(SN_SWITCH_STATEMENT);
	parents_.push_back(item);
	Scope* before = current_;
	if (stmt.condition->is_declaration)
		current_ = model_.CreateScope(SCOPE_BLOCK, "", before);
	BindCondition(*stmt.condition, true);
	BindStatement(*stmt.body);
	current_ = before;
	parents_.pop_back();
	if (wrapper)
		parents_.pop_back();
	current_ = saved;
}

void SemBinder::BindLabelStatement(const AstStmt& stmt)
{
	SemNode* item = AppendItem(stmt.kind == SK_CASE ? SN_CASE_STATEMENT
	                                                : SN_DEFAULT_STATEMENT);
	parents_.push_back(item);
	if (stmt.kind == SK_CASE)
	{
		SemValue value = analyzer_.Analyze(*stmt.expr);
		item->children.push_back(std::move(value.node));
		// 6.4.2p2: the case value is a constant expression; record it
		// for the lowering's dispatch table.
		ConstValue case_value;
		if (!TryEvaluateConstant(*stmt.expr, case_value))
			throw runtime_error("case value is not a constant "
			                    "expression");
		item->has_value = true;
		item->value = case_value;
	}
	BindStatement(*stmt.body);
	parents_.pop_back();
}
