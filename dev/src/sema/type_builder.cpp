#include "sema/type_builder.h"

#include <stdexcept>

using std::runtime_error;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA11 assignment boundary");
}

void SetOnce(bool& flag, const char* what)
{
	if (flag)
		throw runtime_error(string("duplicate ") + what);
	flag = true;
}

bool IsBaseTypeKeyword(ETokenType keyword)
{
	switch (keyword)
	{
	case KW_CHAR: case KW_CHAR16_T: case KW_CHAR32_T: case KW_WCHAR_T:
	case KW_BOOL: case KW_INT: case KW_FLOAT: case KW_DOUBLE: case KW_VOID:
		return true;
	default:
		return false;
	}
}

// 8.3.5p2 with the PA11 (void) normalization: a single unnamed
// parameter of exactly unqualified void means an empty parameter list;
// any other void parameter is ill-formed.
bool NormalizeVoidParameter(const vector<ParameterInfo>& parameters,
                            bool variadic)
{
	bool lone_void = parameters.size() == 1 && !variadic &&
		parameters[0].type->kind == TK_FUNDAMENTAL &&
		parameters[0].type->fundamental == FT_VOID;
	if (lone_void &&
	    (parameters[0].type->is_const || parameters[0].type->is_volatile ||
	     !parameters[0].name.empty()))
		throw runtime_error("invalid void parameter");
	for (size_t i = 0; i < parameters.size(); i++)
		if (!lone_void && IsVoidType(parameters[i].type))
			throw runtime_error("invalid void parameter");
	return lone_void;
}

}  // namespace

TypeBuilder::TypeBuilder(ITypeBuilderHost& host)
	: host_(host)
{
}

void TypeBuilder::ConsumeSpecifierKeyword(const AstSpecifier& spec,
                                          bool allow_storage,
                                          DeclSpecifierInfo& info,
                                          SimpleTypeSpecifiers& simple,
                                          bool& is_const, bool& is_volatile)
{
	bool storage = true;
	switch (spec.keyword)
	{
	case KW_TYPEDEF: SetOnce(info.is_typedef, "typedef"); break;
	case KW_STATIC: SetOnce(info.is_static, "static"); break;
	case KW_EXTERN: SetOnce(info.is_extern, "extern"); break;
	case KW_THREAD_LOCAL: SetOnce(info.is_thread_local, "thread_local"); break;
	case KW_INLINE: SetOnce(info.is_inline, "inline"); break;
	case KW_VIRTUAL: SetOnce(info.is_virtual, "virtual"); break;
	case KW_CONSTEXPR: SetOnce(info.is_constexpr, "constexpr"); break;
	default:
		storage = false;
		break;
	}
	if (storage)
	{
		// 8.3.5p2 (parameters) / 8.4 (type-ids): type-specifiers only.
		if (!allow_storage)
			throw runtime_error("specifier not allowed in this context");
		return;
	}
	switch (spec.keyword)
	{
	case KW_CONST:
		// 7.1.6.2p2: redundant cv is ill-formed except via typedefs.
		SetOnce(is_const, "const");
		break;
	case KW_VOLATILE:
		SetOnce(is_volatile, "volatile");
		break;
	case KW_SIGNED: simple.signed_count++; break;
	case KW_UNSIGNED: simple.unsigned_count++; break;
	case KW_SHORT: simple.short_count++; break;
	case KW_LONG: simple.long_count++; break;
	default:
		if (!IsBaseTypeKeyword(spec.keyword))
			throw OutsideBoundary("decl-specifier keyword");
		if (simple.has_base)
			throw runtime_error("multiple type specifiers");
		simple.has_base = true;
		simple.base = spec.keyword;
		break;
	}
}

DeclSpecifierInfo TypeBuilder::ProcessSpecifiers(const AstSpecifierSeq& seq,
                                                 bool allow_storage)
{
	DeclSpecifierInfo info;
	SimpleTypeSpecifiers simple;
	TypePtr named;
	bool is_const = false;
	bool is_volatile = false;
	for (size_t i = 0; i < seq.size(); i++)
	{
		const AstSpecifier& spec = seq[i];
		TypePtr resolved;
		switch (spec.kind)
		{
		case SPEC_KEYWORD:
			ConsumeSpecifierKeyword(spec, allow_storage, info, simple,
			                        is_const, is_volatile);
			continue;
		case SPEC_TYPE_NAME:
			resolved = host_.ResolveTypeName(spec.name);
			break;
		case SPEC_DECLTYPE:
			resolved = host_.ResolveDecltype(*spec.decltype_expr);
			break;
		case SPEC_NESTED_DECL:
			resolved = host_.BindNestedTypeSpecifier(*spec.nested_decl);
			break;
		}
		if (named)
			throw runtime_error("multiple type specifiers");
		named = resolved;
	}
	if (named && AnySimpleTypeSpecifier(simple))
		throw runtime_error("type-name combined with simple type "
		                    "specifiers");
	if (info.is_typedef &&
	    (info.is_static || info.is_extern || info.is_thread_local ||
	     info.is_constexpr || info.is_inline || info.is_virtual))
		throw runtime_error("typedef combined with another specifier");
	TypePtr base = named ? named
		: MakeFundamentalType(CombineSimpleTypeSpecifiers(simple));
	info.type = MakeCvQualifiedType(base, is_const, is_volatile);
	return info;
}

void TypeBuilder::BuildParameters(const AstParameterClause& clause,
                                  vector<ParameterInfo>& parameters,
                                  vector<TypePtr>& types)
{
	for (size_t i = 0; i < clause.parameters.size(); i++)
	{
		const AstParameter& parameter = clause.parameters[i];
		DeclSpecifierInfo specs =
			ProcessSpecifiers(parameter.specifiers, false);
		DeclaratorInfo composed =
			ComposeDeclarator(parameter.declarator.get(), specs.type);
		ParameterInfo result;
		if (composed.id)
		{
			if (!composed.id->IsPlainIdentifier())
				throw OutsideBoundary("qualified parameter name");
			result.name = composed.id->parts[0].identifier;
		}
		result.type = composed.type;
		parameters.push_back(result);
	}
	if (NormalizeVoidParameter(parameters, clause.variadic))
		parameters.clear();
	for (size_t i = 0; i < parameters.size(); i++)
		types.push_back(parameters[i].type);
}

void TypeBuilder::ApplyDeclaratorSuffix(const AstDeclaratorItem& item,
                                        DeclaratorInfo& out)
{
	switch (item.kind)
	{
	case DI_PARAMS:
	{
		vector<ParameterInfo> parameters;
		vector<TypePtr> types;
		BuildParameters(*item.params, parameters, types);
		out.type = MakeFunctionType(out.type, types, item.params->variadic);
		out.declares_function = true;
		out.parameters.swap(parameters);
		break;
	}
	case DI_ARRAY:
	{
		bool bound_known = item.array_bound.get() != 0;
		unsigned long long bound = bound_known
			? host_.EvaluateArrayBound(*item.array_bound) : 0;
		out.type = MakeArrayType(out.type, bound_known, bound);
		out.declares_function = false;
		break;
	}
	case DI_FUNC_QUAL:
		// Exception specifications do not enter the PA11 type model;
		// virt-specifiers and ref-qualifiers are member-function
		// territory.
		if (item.qual.kind == FQ_VIRT)
			throw OutsideBoundary("virt-specifier or ref-qualifier");
		break;
	default:
		throw OutsideBoundary("declarator form");
	}
}

// One nesting level: leading ptr-operators (with their cv), the core
// (declarator-id or nested declarator), trailing suffixes. The type of
// T D for D = * D1 gives D1 base "pointer to T" (8.3.1), so the
// prefix operators apply to the base left to right; the suffixes bind
// tighter than nesting and compose right to left (8.3.4p3); the nested
// core then recurses over the result (8.3p6).
void TypeBuilder::ComposeItems(const vector<AstDeclaratorItem>& items,
                               bool collapsible, DeclaratorInfo& out)
{
	size_t prefix_end = 0;
	while (prefix_end < items.size() &&
	       (items[prefix_end].kind == DI_PTR ||
	        items[prefix_end].kind == DI_CV))
		prefix_end++;
	size_t core = items.size();
	size_t suffix_begin = prefix_end;
	if (prefix_end < items.size() &&
	    (items[prefix_end].kind == DI_ID ||
	     items[prefix_end].kind == DI_NESTED))
	{
		core = prefix_end;
		suffix_begin = prefix_end + 1;
	}
	for (size_t i = 0; i < prefix_end; i++)
	{
		const AstDeclaratorItem& item = items[i];
		if (item.kind == DI_CV)
			continue;  // consumed by its pointer below
		if (item.token == OP_STAR)
		{
			bool ptr_const = false;
			bool ptr_volatile = false;
			for (size_t j = i + 1;
			     j < prefix_end && items[j].kind == DI_CV; j++)
			{
				if (items[j].token == KW_CONST)
					ptr_const = true;
				else
					ptr_volatile = true;
			}
			out.type = MakePointerType(out.type, ptr_const, ptr_volatile);
		}
		else if (item.token == OP_AMP || item.token == OP_LAND)
			// Collapsing is only legal while the reference target is
			// still the specifier-seq base, i.e. came through a
			// typedef-name or decltype (8.3.2p6).
			out.type = MakeReferenceType(out.type, item.token == OP_LAND,
			                             collapsible);
		else
			throw OutsideBoundary("ptr-operator");
		collapsible = false;
		out.declares_function = false;
	}
	for (size_t i = items.size(); i > suffix_begin; i--)
	{
		ApplyDeclaratorSuffix(items[i - 1], out);
		collapsible = false;
	}
	if (core == items.size())
		return;
	if (items[core].kind == DI_ID)
	{
		out.id = &items[core].name;
		return;
	}
	ComposeItems(items[core].nested->items, collapsible, out);
}

DeclaratorInfo TypeBuilder::ComposeDeclarator(const AstDeclarator* declarator,
                                              const TypePtr& base)
{
	DeclaratorInfo info;
	info.type = base;
	if (declarator)
		ComposeItems(declarator->items, true, info);
	return info;
}

TypePtr TypeBuilder::ResolveTypeId(const AstTypeId& type_id)
{
	DeclSpecifierInfo specs = ProcessSpecifiers(type_id.specifiers, false);
	DeclaratorInfo composed =
		ComposeDeclarator(type_id.declarator.get(), specs.type);
	if (composed.id)
		throw runtime_error("type-id requires an abstract declarator");
	return composed.type;
}
