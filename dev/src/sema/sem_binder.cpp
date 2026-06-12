#include "sema/sem_binder.h"

#include <stdexcept>

using std::runtime_error;
using std::to_string;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA12 assignment boundary");
}

const AstExpr* StripParens(const AstExpr* expr)
{
	while (expr->kind == EK_PAREN)
		expr = expr->operands[0].get();
	return expr;
}

// 13.1p2: declarations naming the same parameter list must agree
// exactly (a return-type-only difference cannot overload).
bool SameParameterList(const Type& a, const Type& b)
{
	if (a.variadic != b.variadic ||
	    a.parameters.size() != b.parameters.size() ||
	    a.is_const != b.is_const || a.is_volatile != b.is_volatile ||
	    a.ref_qual != b.ref_qual)
		return false;
	for (size_t i = 0; i < a.parameters.size(); i++)
		if (!TypeEquals(a.parameters[i], b.parameters[i]))
			return false;
	return true;
}

}  // namespace

SemBinder::SemBinder(TypesModel& model, SemUnit& unit)
	: DeclBinder(model), unit_(unit), analyzer_(*this),
	  local_types_(0), pending_local_type_(false), in_bit_field_(false)
{
	builder_.SetParameterAdjustment(true);
	// The PA12 grammar uses nullptr_t as a built-in type name.
	ScopeBinding nullptr_alias;
	nullptr_alias.kind = SB_TYPE_ALIAS;
	nullptr_alias.name = "nullptr_t";
	nullptr_alias.type = MakeFundamentalType(FT_NULLPTR_T);
	AddBinding(*model.global(), nullptr_alias);
	// 3.7.4: the global allocation and deallocation functions are
	// implicitly declared in every translation unit.
	TypePtr size_type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	TypePtr byte_ptr = MakePointerType(MakeFundamentalType(FT_VOID),
	                                   false, false);
	TypePtr void_type = MakeFundamentalType(FT_VOID);
	const char* alloc_names[2] = {"operator new", "operator new[]"};
	const char* free_names[2] = {"operator delete", "operator delete[]"};
	for (int i = 0; i < 2; i++)
	{
		ScopeBinding alloc;
		alloc.kind = SB_FUNCTION;
		alloc.name = alloc_names[i];
		vector<TypePtr> params;
		params.push_back(size_type);
		alloc.type = MakeFunctionType(byte_ptr, params, false);
		alloc.fn_defaults.resize(1);
		alloc.fn_deleted.resize(1, false);
		alloc.fn_access.resize(1, MA_PUBLIC);
		alloc.fn_static.resize(1, false);
		alloc.fn_inline_def.resize(1, false);
		alloc.fn_adl_only.resize(1, false);
		alloc.fn_unwind_no.resize(1, false);
		AddBinding(*model.global(), alloc);
		ScopeBinding dealloc;
		dealloc.kind = SB_FUNCTION;
		dealloc.name = free_names[i];
		vector<TypePtr> free_params;
		free_params.push_back(byte_ptr);
		dealloc.type = MakeFunctionType(void_type, free_params, false);
		dealloc.fn_defaults.resize(1);
		dealloc.fn_deleted.resize(1, false);
		dealloc.fn_access.resize(1, MA_PUBLIC);
		dealloc.fn_static.resize(1, false);
		dealloc.fn_inline_def.resize(1, false);
		dealloc.fn_adl_only.resize(1, false);
		dealloc.fn_unwind_no.resize(1, true);
		AddBinding(*model.global(), dealloc);
	}
}

// --- ISemExprHost ----------------------------------------------------------

Scope* SemBinder::CurrentScope()
{
	return current_;
}

TypesModel& SemBinder::Model()
{
	return model_;
}

const ScopeBinding* SemBinder::ResolveValue(const AstName& name,
                                            const NamedTypeInfo*& member_class)
{
	member_class = 0;
	const string& terminal = TerminalName(name);
	if (name.parts.size() == 1 && !name.global_scope)
	{
		const ScopeBinding* found =
			UnqualifiedLookup(current_, terminal, SLF_ANY);
		if (!found)
			throw runtime_error("undeclared name " + terminal);
		return found;
	}
	Scope* scope = ResolvePrefixScope(name);
	const ScopeBinding* found = QualifiedLookup(*scope, terminal, SLF_ANY);
	if (!found)
		throw runtime_error("no member named " + terminal);
	if (scope->kind == SCOPE_CLASS)
		member_class = model_.ScopeEntity(scope);
	return found;
}

TypePtr SemBinder::TryResolveCalleeType(const AstName& name)
{
	if (!name.global_scope && name.parts.size() == 1 &&
	    name.parts[0].kind == NP_DECLTYPE)
		return ResolveDecltype(*name.parts[0].decltype_expr);
	return TryResolveTypeFromName(name);
}

TypePtr SemBinder::ResolveCastTypeId(const AstTypeId& type_id)
{
	return builder_.ResolveTypeId(type_id);
}

const ScopeBinding* SemBinder::ResolveBuiltinFunction(const string& name)
{
	TypePtr void_type = MakeFundamentalType(FT_VOID);
	TypePtr size_type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	TypePtr byte_ptr = MakePointerType(MakeFundamentalType(FT_VOID),
	                                   false, false);
	TypePtr const_char_ptr = MakePointerType(
		MakeCvQualifiedType(MakeFundamentalType(FT_CHAR), true, false),
		false, false);
	TypePtr type;
	vector<TypePtr> params;
	if (name == "__builtin_strlen")
	{
		params.push_back(const_char_ptr);
		type = MakeFunctionType(size_type, params, false);
	}
	else if (name == "__builtin_memcpy" || name == "__builtin_memmove")
	{
		params.push_back(byte_ptr);
		params.push_back(MakePointerType(
			MakeCvQualifiedType(MakeFundamentalType(FT_VOID), true,
			                    false), false, false));
		params.push_back(size_type);
		type = MakeFunctionType(byte_ptr, params, false);
	}
	else if (name == "__builtin_unreachable")
		type = MakeFunctionType(void_type, params, false);
	else
		return 0;
	if (const ScopeBinding* existing =
	        FindOwnBinding(*model_.global(), name))
		return existing;
	ScopeBinding binding;
	binding.kind = SB_FUNCTION;
	binding.name = name;
	binding.type = type;
	binding.c_linkage = true;
	binding.fn_defaults.resize(1);
	binding.fn_deleted.resize(1, false);
	binding.fn_access.resize(1, MA_PUBLIC);
	binding.fn_static.resize(1, false);
	binding.fn_inline_def.resize(1, false);
	binding.fn_adl_only.resize(1, false);
	binding.fn_unwind_no.resize(1, true);
	return &AddBinding(*model_.global(), binding);
}

bool SemBinder::TryEvaluateConstant(const AstExpr& expr, ConstValue& value)
{
	try
	{
		value = EvaluateConstExpr(expr, *this);
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}

TypePtr SemBinder::ResolveDecltype(const AstExpr& expr)
{
	// 7.1.6.2p4: an unparenthesized id-expression yields the declared
	// type (the PA11 rule); any other operand classifies as an
	// expression: T& for lvalues, T&& for xvalues, T for prvalues.
	if (StripParens(&expr)->kind == EK_ID)
		return DeclBinder::ResolveDecltype(expr);
	SemValue value = analyzer_.Analyze(expr);
	if (value.category == VC_LVALUE)
		return MakeReferenceType(value.type, false, true);
	if (value.category == VC_XVALUE)
		return MakeReferenceType(value.type, true, true);
	return value.type;
}

// --- dump recording ----------------------------------------------------------

SemNode* SemBinder::AppendItem(SemNodePtr node)
{
	vector<SemNodePtr>& list =
		parents_.empty() ? unit_.items : parents_.back()->children;
	list.push_back(std::move(node));
	return list.back().get();
}

SemNode* SemBinder::AppendItem(ESemNodeKind kind)
{
	return AppendItem(MakeSemNode(kind));
}

// --- declaration seams --------------------------------------------------------

void SemBinder::BindNamespaceBody(const AstDecl& decl, Scope* scope)
{
	SemNode* item = AppendItem(SN_NAMESPACE_DEFINITION);
	item->name = decl.unnamed ? "<unnamed>" : decl.name;
	parents_.push_back(item);
	DeclBinder::BindNamespaceBody(decl, scope);
	parents_.pop_back();
}

void SemBinder::BindSimpleDeclaration(const AstDecl& decl)
{
	// An anonymous class used by a declarator gets the deterministic
	// __local_type<n> entity name (pinned by the PA12 fixtures).
	bool pending = false;
	if (!decl.declarators.empty())
		for (size_t i = 0; i < decl.specifiers.size(); i++)
		{
			const AstSpecifier& spec = decl.specifiers[i];
			if (spec.kind == SPEC_NESTED_DECL &&
			    spec.nested_decl->kind == DK_CLASS &&
			    !spec.nested_decl->has_name)
				pending = true;
		}
	pending_local_type_ = pending;
	DeclBinder::BindSimpleDeclaration(decl);
	pending_local_type_ = false;
}

string SemBinder::AnonymousTypeName(const AstDecl& decl)
{
	if (pending_local_type_)
		return "__local_type" + to_string(++local_types_);
	return DeclBinder::AnonymousTypeName(decl);
}

// The enclosing named namespace/class path of `scope` joined with `::`
// (trailing separator included); unnamed components are skipped.
string SemBinder::QualifiedScopePath(const Scope* scope) const
{
	string path;
	for (; scope && scope->parent; scope = scope->parent)
	{
		if ((scope->kind == SCOPE_NAMESPACE ||
		     scope->kind == SCOPE_CLASS) && !scope->name.empty())
			path = scope->name + "::" + path;
	}
	return path;
}

string SemBinder::TypeDisplayName(const string& key,
                                  const string& name) const
{
	// PA12 displays are qualified by the enclosing named namespaces and
	// classes ("struct n::S"); unnamed namespace components are skipped.
	return key + " " + QualifiedScopePath(current_) + name;
}

ScopeBinding& SemBinder::BindFunctionName(const string& name,
                                          const TypePtr& type,
                                          bool allow_block)
{
	ScopeBinding* existing = FindOwnBinding(*current_, name);
	if (!existing)
		return DeclBinder::BindFunctionName(name, type, allow_block);
	if (existing->kind != SB_FUNCTION ||
	    (current_->kind != SCOPE_NAMESPACE &&
	     current_->kind != SCOPE_CLASS &&
	     !(allow_block && current_->kind == SCOPE_BLOCK)))
		throw runtime_error("redeclaration of " + name);
	// 13.1: a matching parameter list redeclares (and must agree in
	// full); a new parameter list extends the overload set. An own
	// class member replaces a same-signature using-import (7.3.3p15).
	if (SameParameterList(*existing->type, *type))
	{
		if (current_->kind == SCOPE_CLASS && existing->owner != current_)
		{
			existing->type = type;
			existing->owner = current_;
			return *existing;
		}
		existing->type = MergeRedeclaredType(existing->type, type);
		return *existing;
	}
	for (size_t i = 0; i < existing->overloads.size(); i++)
	{
		if (!SameParameterList(*existing->overloads[i], *type))
			continue;
		existing->overloads[i] =
			MergeRedeclaredType(existing->overloads[i], type);
		return *existing;
	}
	existing->overloads.push_back(type);
	return *existing;
}

void SemBinder::OnTypeAliasBound(const string& name, const TypePtr& type)
{
	if (current_->kind == SCOPE_CLASS)
		return;
	SemNode* item = AppendItem(SN_TYPE_ALIAS);
	item->name = name;
	item->type = type;
}

void SemBinder::OnFunctionDeclared(ScopeBinding& binding,
                                   const TypePtr& type)
{
	if (current_->kind == SCOPE_CLASS)
		return;
	SemNode* item = AppendItem(SN_FUNCTION_DECLARATION);
	item->name = CanonicalQualifiedName(binding.owner, binding.name);
	item->type = type;
	item->entity_scope = binding.owner;
	item->entity_name = binding.name;
	item->c_linkage = binding.c_linkage;
}

void SemBinder::OnVariableBound(ScopeBinding& binding,
                                const AstInitializer* init,
                                const DeclSpecifierInfo& specs)
{
	if (current_->kind == SCOPE_CLASS)
	{
		RecordMemberField(binding, init, specs);
		return;
	}
	SemNode* item = AppendItem(SN_VARIABLE);
	item->name = binding.name;
	item->type = binding.type;
	item->entity_scope = binding.owner;
	item->entity_name = binding.name;
	item->is_static_decl = specs.is_static;
	item->is_extern_decl = specs.is_extern;
	item->is_thread_local_decl = specs.is_thread_local;
	item->c_linkage = in_c_linkage_;
	AttachObjectLifetime(*item, binding, init, specs);
}

void SemBinder::AnalyzeVariableInit(SemNode& item, ScopeBinding& binding,
                                    const AstInitializer* init)
{
	const AstExpr* expr = 0;
	const AstExpr* braced = 0;
	switch (init->kind)
	{
	case INIT_EQ:
		if (init->expr->kind == EK_BRACED)
			braced = init->expr.get();
		else
			expr = init->expr.get();
		break;
	case INIT_PAREN:
		if (init->args.size() != 1)
			throw OutsideBoundary("paren-initializer form");
		expr = init->args[0].get();
		break;
	case INIT_BRACED:
		braced = init->expr.get();
		break;
	default:
		throw OutsideBoundary("initializer form");
	}
	if (braced)
	{
		TypePtr completed = binding.type;
		item.children.push_back(analyzer_.AnalyzeBracedInit(*braced,
		                                                    completed));
		binding.type = completed;
		item.type = completed;
		return;
	}
	SemValue value = analyzer_.Analyze(*expr);
	if (init->kind == INIT_PAREN)
	{
		// 8.5p15: direct-initialization admits explicit conversion
		// functions.
		ImplicitConversion conv = ClassifyConversionEx(
			MakeConversionSource(value), binding.type, true);
		if (!conv.viable)
			throw runtime_error("no conversion for initialization");
		analyzer_.ApplyConversion(value, conv, binding.type);
	}
	else
		analyzer_.CopyInitialize(value, binding.type, "initialization");
	item.children.push_back(std::move(value.node));
}

const string& SemBinder::EnsureDefaultConstructor(const TypePtr& type,
                                                  TypePtr& ctor_type)
{
	const NamedTypeInfo* info = type->named;
	if (!info->complete)
		throw runtime_error(info->display + " is an incomplete type");
	vector<TypePtr> parameters;
	parameters.push_back(
		MakePointerType(MakeNamedType(TK_CLASS, info), false, false));
	ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID), parameters,
	                             false);
	std::map<const NamedTypeInfo*, string>::iterator found =
		constructors_.find(info);
	if (found != constructors_.end())
		return found->second;
	// The qualified constructor name: the class's enclosing scope path
	// plus its own name twice. The member scope records both facts (a
	// complete class always has one).
	const Scope* members = model_.MemberScope(info);
	const string& base = members->name;
	string name = QualifiedScopePath(members->parent) + base + "::" + base;

	SemNodePtr definition = MakeSemNode(SN_FUNCTION_DEFINITION);
	definition->name = name;
	definition->type = ctor_type;
	SemNodePtr this_param = MakeSemNode(SN_PARAMETER);
	this_param->name = "this";
	this_param->type = parameters[0];
	definition->children.push_back(std::move(this_param));
	definition->children.push_back(MakeSemNode(SN_COMPOUND_STATEMENT));
	unit_.synthesized.push_back(std::move(definition));
	return constructors_[info] = name;
}

void SemBinder::EmitConstructorAction(SemNode& item, const string& var_name,
                                      const TypePtr& type)
{
	TypePtr ctor_type;
	const string& ctor_name = EnsureDefaultConstructor(type, ctor_type);

	SemNodePtr action = MakeSemNode(SN_CONSTRUCTOR_ACTION);
	action->name = ctor_name;
	SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
	call->type = MakeFundamentalType(FT_VOID);
	call->category = VC_PRVALUE;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = ctor_name;
	callee->type = ctor_type;
	SemNodePtr address = MakeSemNode(SN_UNARY_EXPRESSION);
	address->type = MakePointerType(type, false, false);
	address->category = VC_PRVALUE;
	address->has_op = true;
	address->op = OP_AMP;
	address->op_spelling = "&";
	SemNodePtr object = MakeSemNode(SN_ID_EXPRESSION);
	object->name = var_name;
	object->type = type;
	object->category = VC_LVALUE;
	address->children.push_back(std::move(object));
	call->children.push_back(std::move(callee));
	call->children.push_back(std::move(address));
	action->children.push_back(std::move(call));
	item.children.push_back(std::move(action));
}

void SemBinder::BindAnonymousUnionMembers(const AstDecl& decl,
                                          const TypePtr& type,
                                          const Scope& union_scope)
{
	// 9.5p5 with the PA12 dump: the unnamed object gets a deterministic
	// storage variable, and the injected members remember it so their
	// uses dump as member accesses.
	string storage_name = "__anonymous_union_storage__" +
		to_string(decl.begin_token) + "_" + to_string(decl.end_token);
	ScopeBinding storage;
	storage.kind = SB_VARIABLE;
	storage.name = storage_name;
	storage.type = type;
	AddBinding(*current_, storage);

	if (current_->kind != SCOPE_CLASS)
	{
		SemNode* item = AppendItem(SN_VARIABLE);
		item->name = storage_name;
		item->type = type;
		EmitConstructorAction(*item, storage_name, type);
	}
	ClassInfo* enclosing = OpenClass();
	const ClassField* anon_row = 0;
	if (current_->kind == SCOPE_CLASS && enclosing &&
	    current_ == enclosing->members)
	{
		// The anonymous object occupies one field row; its members
		// inject at absolute offsets so every access addresses the
		// enclosing object directly.
		ClassField anon_field;
		anon_field.name = storage_name;
		anon_field.type = type;
		anon_field.access = current_access_;
		anon_row = &LayoutField(*enclosing, anon_field);
	}
	const ClassInfo* anon_cls =
		anon_row ? unit_.classes.Find(type->named) : 0;
	for (size_t i = 0; i < union_scope.bindings.size(); i++)
	{
		const ScopeBinding& member = union_scope.bindings[i];
		// The implicitly declared assignment operators live in the
		// union's member scope but are not data members.
		if (member.kind == SB_FUNCTION && member.name == "operator =")
			continue;
		if (member.kind != SB_VARIABLE)
			throw runtime_error("anonymous union member is not a "
			                    "non-static data member");
		ScopeBinding injected = member;
		injected.owner = 0;  // re-stamped by AddBinding
		injected.anon_storage_name = storage_name;
		injected.anon_storage_type = type;
		AddBinding(*current_, injected);
		if (anon_cls)
		{
			const ClassField* inner =
				FindClassField(*anon_cls, member.name);
			if (inner)
			{
				ClassField row = *inner;
				row.offset += anon_row->offset;
				row.access = current_access_;
				enclosing->fields.push_back(row);
			}
		}
	}
}

void SemBinder::BindFunctionBody(const AstDecl& decl,
                                 const DeclaratorInfo& composed,
                                 const string& name)
{
	// current_ is the new function scope; its parent declared the name.
	const Scope* declaring = current_->parent;
	if (declaring && declaring->kind == SCOPE_CLASS)
	{
		BindMemberFunctionBody(decl, composed, name);
		return;
	}
	SemNode* item = AppendItem(SN_FUNCTION_DEFINITION);
	item->name = CanonicalQualifiedName(declaring, name);
	item->type = composed.type;
	item->entity_scope = declaring;
	item->entity_name = name;
	item->unwind_no = composed.noexcept_simple;
	if (const ScopeBinding* fn = FindOwnBinding(*declaring, name))
		item->c_linkage = fn->c_linkage;
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = composed.parameters[i].name;
		parameter->type = composed.parameters[i].type;
		parameter->entity_scope = current_;
		parameter->entity_name = composed.parameters[i].name;
		item->children.push_back(std::move(parameter));
	}
	parents_.push_back(item);
	TypePtr saved_return = current_return_;
	MethodContext saved_method = method_;
	method_ = MethodContext();
	method_.fn_scope = current_;
	method_.fn_owner = declaring;
	method_.fn_name = name;
	current_return_ = composed.type->target;
	DeclBinder::BindFunctionBody(decl, composed, name);
	current_return_ = saved_return;
	method_ = saved_method;
	parents_.pop_back();
}

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

void SemBinder::BindReturnStatement(const AstStmt& stmt)
{
	SemNode* item = AppendItem(SN_RETURN_STATEMENT);
	if (!stmt.expr)
	{
		if (!IsVoidType(current_return_))
			throw runtime_error("non-void function returns no value");
		return;
	}
	SemValue value = analyzer_.Analyze(*stmt.expr);
	TypePtr bare = RemoveTopCv(current_return_);
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
	SemNode* item = AppendItem(SN_IF_STATEMENT);
	parents_.push_back(item);
	Scope* saved = current_;
	if (stmt.condition->is_declaration)
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
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
	current_ = saved;
	parents_.pop_back();
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
	SemNode* item = AppendItem(SN_SWITCH_STATEMENT);
	parents_.push_back(item);
	Scope* saved = current_;
	if (stmt.condition->is_declaration)
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
	BindCondition(*stmt.condition, true);
	BindStatement(*stmt.body);
	current_ = saved;
	parents_.pop_back();
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
