#include "sema/sem_binder.h"

#include <set>
#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;
using std::set;

// PA18 definition-time template sanity (14.6p7, 14.6.1p6, 14.6.3): a
// captured template body is never semantically analyzed until
// instantiation, but two spec-anchored classes of errors must be
// rejected at the definition itself: a declaration that redeclares a
// template-parameter name, and a non-dependent unqualified name that
// resolves nowhere. The walk is deliberately conservative — silence is
// always allowed; only the certain cases throw. Unresolved names are
// re-checked at the end of the translation unit so names declared
// later (which instantiation-time lookup would find) never trip it.

namespace {

struct BodyNames
{
	BodyNames() : give_up(false) {}

	// Names any unqualified id may legitimately mean without lookup:
	// template parameters, members of the (possibly current) class,
	// parameters, and every name declared anywhere in the walked body.
	set<string> params;
	set<string> known;
	// A body-level using-directive or using-declaration makes local
	// visibility unknowable here; ids stop being collected (the
	// parameter-shadow check still runs).
	bool give_up;
};

void CollectDeclaratorName(const AstDeclarator* declarator,
                           BodyNames& names, bool check_shadow)
{
	if (!declarator)
		return;
	const AstName* id = declarator->IdName();
	if (!id || !id->IsPlainIdentifier())
		return;
	const string& name = id->parts[0].identifier;
	if (check_shadow && names.params.count(name))
		throw runtime_error("declaration of " + name +
		                    " shadows a template parameter");
	names.known.insert(name);
}

void CollectParameterNames(const AstDeclarator* declarator,
                           BodyNames& names)
{
	if (!declarator)
		return;
	for (size_t i = 0; i < declarator->items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator->items[i];
		if (item.kind == DI_NESTED && item.nested)
			CollectParameterNames(item.nested.get(), names);
		if (item.kind != DI_PARAMS || !item.params)
			continue;
		for (size_t p = 0; p < item.params->parameters.size(); p++)
			CollectDeclaratorName(
				item.params->parameters[p].declarator.get(), names,
				false);
	}
}

void WalkStmt(const AstStmt* stmt, BodyNames& names,
              vector<const AstExpr*>& ids);

void WalkExpr(const AstExpr* expr, BodyNames& names,
              vector<const AstExpr*>& ids)
{
	if (!expr)
		return;
	if (expr->kind == EK_ID)
	{
		// Unqualified plain identifiers only; a callee id is exempt
		// (argument-dependent lookup may find it at instantiation).
		if (!expr->name.global_scope && expr->name.parts.size() == 1 &&
		    expr->name.parts[0].kind == NP_IDENTIFIER &&
		    !expr->name.parts[0].tilde)
			ids.push_back(expr);
		return;
	}
	size_t skip_callee = 0;
	if (expr->kind == EK_CALL && !expr->operands.empty() &&
	    expr->operands[0] && expr->operands[0]->kind == EK_ID)
		skip_callee = 1;
	for (size_t i = skip_callee; i < expr->operands.size(); i++)
		WalkExpr(expr->operands[i].get(), names, ids);
	for (size_t i = 0; i < expr->arguments.size(); i++)
		WalkExpr(expr->arguments[i].get(), names, ids);
	if (expr->new_init && expr->new_init->expr)
		WalkExpr(expr->new_init->expr.get(), names, ids);
}

void WalkLocalDecl(const AstDecl* decl, BodyNames& names,
                   vector<const AstExpr*>& ids)
{
	if (!decl)
		return;
	if (decl->kind == DK_ALIAS)
	{
		if (names.params.count(decl->name))
			throw runtime_error("declaration of " + decl->name +
			                    " shadows a template parameter");
		names.known.insert(decl->name);
		return;
	}
	if (decl->kind == DK_CLASS || decl->kind == DK_CLASS_FORWARD ||
	    decl->kind == DK_ENUM)
	{
		// Local classes and enums: their names (and enumerators) join
		// the known set; member details stay unchecked.
		if (decl->has_name && decl->class_name.IsPlainIdentifier())
			names.known.insert(decl->class_name.parts[0].identifier);
		if (!decl->name.empty())
			names.known.insert(decl->name);
		for (size_t i = 0; i < decl->enumerators.size(); i++)
			names.known.insert(decl->enumerators[i].name);
		for (size_t i = 0; i < decl->members.size(); i++)
			WalkLocalDecl(decl->members[i].get(), names, ids);
		return;
	}
	if (decl->kind == DK_USING_DIRECTIVE ||
	    decl->kind == DK_USING_DECLARATION)
	{
		names.give_up = true;
		return;
	}
	if (decl->kind != DK_SIMPLE)
		return;
	for (size_t i = 0; i < decl->declarators.size(); i++)
	{
		CollectDeclaratorName(decl->declarators[i].declarator.get(),
		                      names, true);
		const AstInitializer* init = decl->declarators[i].init.get();
		if (!init)
			continue;
		if (init->expr)
			WalkExpr(init->expr.get(), names, ids);
		for (size_t a = 0; a < init->args.size(); a++)
			WalkExpr(init->args[a].get(), names, ids);
	}
}

void WalkCondition(const AstCondition* condition, BodyNames& names,
                   vector<const AstExpr*>& ids)
{
	if (!condition)
		return;
	if (condition->is_declaration)
	{
		CollectDeclaratorName(condition->declarator.get(), names, true);
		if (condition->init && condition->init->expr)
			WalkExpr(condition->init->expr.get(), names, ids);
		return;
	}
	WalkExpr(condition->expr.get(), names, ids);
}

void WalkStmt(const AstStmt* stmt, BodyNames& names,
              vector<const AstExpr*>& ids)
{
	if (!stmt)
		return;
	for (size_t i = 0; i < stmt->items.size(); i++)
		WalkStmt(stmt->items[i].get(), names, ids);
	if (stmt->decl)
		WalkLocalDecl(stmt->decl.get(), names, ids);
	WalkExpr(stmt->expr.get(), names, ids);
	WalkCondition(stmt->condition.get(), names, ids);
	WalkStmt(stmt->then_branch.get(), names, ids);
	WalkStmt(stmt->else_branch.get(), names, ids);
	WalkStmt(stmt->body.get(), names, ids);
	WalkStmt(stmt->for_init.get(), names, ids);
	WalkExpr(stmt->iteration.get(), names, ids);
	for (size_t i = 0; i < stmt->handlers.size(); i++)
	{
		CollectDeclaratorName(stmt->handlers[i].declarator.get(), names,
		                      false);
		WalkStmt(stmt->handlers[i].body.get(), names, ids);
	}
}

// The member names a class pattern declares (any member body may name
// them unqualified, regardless of declaration order, 9.2p2).
void CollectClassMemberNames(const AstDecl& pattern, BodyNames& names)
{
	if (pattern.has_name && pattern.class_name.IsPlainIdentifier())
		names.known.insert(pattern.class_name.parts[0].identifier);
	for (size_t i = 0; i < pattern.members.size(); i++)
	{
		const AstDecl& member = *pattern.members[i];
		switch (member.kind)
		{
		case DK_SIMPLE:
			for (size_t d = 0; d < member.declarators.size(); d++)
				CollectDeclaratorName(
					member.declarators[d].declarator.get(), names,
					false);
			break;
		case DK_FUNCTION:
			CollectDeclaratorName(member.declarator.get(), names,
			                      false);
			break;
		case DK_CLASS:
		case DK_CLASS_FORWARD:
			if (member.has_name &&
			    member.class_name.IsPlainIdentifier())
				names.known.insert(
					member.class_name.parts[0].identifier);
			else if (!member.has_name)
				// 9.5p5: anonymous-union members inject into the
				// enclosing class.
				CollectClassMemberNames(member, names);
			break;
		case DK_ENUM:
			if (!member.name.empty())
				names.known.insert(member.name);
			for (size_t e = 0; e < member.enumerators.size(); e++)
				names.known.insert(member.enumerators[e].name);
			break;
		case DK_ALIAS:
			names.known.insert(member.name);
			break;
		case DK_BIT_FIELD:
			for (size_t b = 0; b < member.bit_fields.size(); b++)
				CollectDeclaratorName(
					member.bit_fields[b].declarator.get(), names,
					false);
			break;
		default:
			break;
		}
	}
}

// One member body: shadow errors throw; unresolved plain ids append to
// the template's re-check list.
void CheckBody(const AstDecl& definition, BodyNames names,
               TemplateInfo& tmpl)
{
	vector<const AstExpr*> ids;
	CollectParameterNames(definition.declarator.get(), names);
	if (definition.body)
		WalkStmt(definition.body.get(), names, ids);
	for (size_t i = 0; i < definition.mem_initializers.size(); i++)
	{
		const AstMemInitializer& init = definition.mem_initializers[i];
		if (init.init && init.init->expr)
			WalkExpr(init.init->expr.get(), names, ids);
	}
	if (names.give_up)
		return;
	for (size_t i = 0; i < ids.size(); i++)
	{
		const string& name = ids[i]->name.parts[0].identifier;
		if (names.params.count(name) || names.known.count(name))
			continue;
		tmpl.suspicious_names.push_back(name);
	}
}

}  // namespace

void SemBinder::CheckTemplateDefinitionSanity(TemplateInfo& tmpl)
{
	BodyNames names;
	for (size_t i = 0; i < tmpl.params.size(); i++)
		if (!tmpl.params[i].name.empty())
			names.params.insert(tmpl.params[i].name);
	const AstDecl& pattern = *tmpl.pattern_decl;
	if (tmpl.kind == TMPL_FUNCTION)
	{
		// 14.3p1: a signature's pack-expansion arguments must target
		// parameter packs (checkable without instantiation).
		ValidateSignatureTemplateIds(pattern.specifiers,
		                             pattern.kind == DK_FUNCTION
		                                 ? pattern.declarator.get()
		                                 : 0);
		if (pattern.kind == DK_FUNCTION)
			CheckBody(pattern, names, tmpl);
		return;
	}
	CollectClassMemberNames(pattern, names);
	// A dependent base defers every unqualified value name (its scope
	// is unknowable here; 14.6.2p3 exclusion stays instantiation-side).
	if (!pattern.bases.empty())
		return;
	for (size_t i = 0; i < pattern.members.size(); i++)
	{
		const AstDecl& member = *pattern.members[i];
		if ((member.kind == DK_FUNCTION ||
		     member.kind == DK_SPECIAL_MEMBER_DEFINITION) &&
		    member.body)
			CheckBody(member, names, tmpl);
	}
}

void SemBinder::FinishTemplateChecks()
{
	const vector<unique_ptr<TemplateInfo>>& all = unit_.templates.All();
	for (size_t t = 0; t < all.size(); t++)
	{
		TemplateInfo& tmpl = *all[t];
		for (size_t i = 0; i < tmpl.suspicious_names.size(); i++)
		{
			const string& name = tmpl.suspicious_names[i];
			if (!UnqualifiedLookup(tmpl.declaring, name, SLF_ANY))
				throw runtime_error("non-dependent name " + name +
				                    " in template " + tmpl.name +
				                    " does not resolve");
		}
		// An odr-used specialization still undefined here is not an
		// error (14.7.1: no diagnostic required; the checked-in
		// compile-pass tests call declared-only templates): the
		// lowering emits an ordinary external declaration for it, like
		// any other declared-but-undefined function.
	}
}

void SemBinder::BindTranslationUnit(const AstDecl& unit)
{
	DeclBinder::BindTranslationUnit(unit);
	FinishTemplateChecks();
}
