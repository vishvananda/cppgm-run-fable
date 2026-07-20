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

// The signed/unsigned counterpart of an integer base type spelled as
// an identifier (__int128, _BitInt containers).
EFundamentalType SignAdjustedFundamental(EFundamentalType type,
                                         bool make_unsigned)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
		return make_unsigned ? FT_UNSIGNED_CHAR : FT_SIGNED_CHAR;
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
		return make_unsigned ? FT_UNSIGNED_SHORT_INT : FT_SHORT_INT;
	case FT_INT:
	case FT_UNSIGNED_INT:
		return make_unsigned ? FT_UNSIGNED_INT : FT_INT;
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
		return make_unsigned ? FT_UNSIGNED_LONG_LONG_INT
		                     : FT_LONG_LONG_INT;
	case FT_INT128:
	case FT_UINT128:
		return make_unsigned ? FT_UINT128 : FT_INT128;
	default:
		throw runtime_error("invalid type specifier combination");
	}
}

}  // namespace

TypeBuilder::TypeBuilder(ITypeBuilderHost& host)
	: host_(host), adjust_parameters_(false)
{
}

// Clang/C23 _BitInt(N): a bit-precise integer accepted with its
// smallest power-of-two container type (size, alignment, and the
// value range of hosted uses up to 128 bits match the x86-64 ABI
// containers; sub-container wrap-at-N arithmetic is a documented
// boundary).
TypePtr TypeBuilder::ResolveBitIntSpecifier(const AstSpecifier& spec)
{
	unsigned long long width =
		host_.EvaluateArrayBound(*spec.decltype_expr);
	EFundamentalType type;
	if (width == 0)
		throw runtime_error("_BitInt width must be positive");
	else if (width <= 8)
		type = FT_SIGNED_CHAR;
	else if (width <= 16)
		type = FT_SHORT_INT;
	else if (width <= 32)
		type = FT_INT;
	else if (width <= 64)
		type = FT_LONG_LONG_INT;
	else if (width <= 128)
		type = FT_INT128;
	else
		throw runtime_error("_BitInt width above 128 bits");
	return MakeFundamentalType(type);
}

void TypeBuilder::SetParameterAdjustment(bool adjust)
{
	adjust_parameters_ = adjust;
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
	case KW_FRIEND: SetOnce(info.is_friend, "friend"); break;
	case KW_MUTABLE: SetOnce(info.is_mutable, "mutable"); break;
	case KW_AUTO: SetOnce(info.is_auto, "auto"); break;
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
	bool sign_combinable = false;
	bool is_const = false;
	bool is_volatile = false;
	bool complex_form = false;
	for (size_t i = 0; i < seq.size(); i++)
	{
		const AstSpecifier& spec = seq[i];
		TypePtr resolved;
		switch (spec.kind)
		{
		case SPEC_COMPLEX:
			// PA34 GNU _Complex/__complex__: wraps the combined
			// element type below.
			complex_form = true;
			continue;
		case SPEC_KEYWORD:
			ConsumeSpecifierKeyword(spec, allow_storage, info, simple,
			                        is_const, is_volatile);
			continue;
		case SPEC_TYPE_NAME:
			resolved = host_.ResolveTypeName(spec.name);
			// GNU __int128: the identifier-spelled base type accepts
			// sign keywords on either side.
			if (spec.name.IsPlainIdentifier() &&
			    spec.name.parts[0].identifier == "__int128")
				sign_combinable = true;
			break;
		case SPEC_DECLTYPE:
			// GNU typeof: a type operand resolves directly; either
			// form strips references (the GNU operator never yields
			// a reference type).
			if (spec.transform_type)
				resolved = ResolveTypeId(*spec.transform_type);
			else
				resolved = host_.ResolveDecltype(*spec.decltype_expr);
			if (spec.typeof_strip && resolved &&
			    IsReferenceType(resolved))
				resolved = resolved->target;
			break;
		case SPEC_NESTED_DECL:
			resolved = host_.BindNestedTypeSpecifier(*spec.nested_decl);
			break;
		case SPEC_TRANSFORM:
			resolved = ResolveTransformSpecifier(spec);
			break;
		case SPEC_BITINT:
			resolved = ResolveBitIntSpecifier(spec);
			sign_combinable = true;
			break;
		}
		if (named)
			throw runtime_error("multiple type specifiers");
		named = resolved;
	}
	if (named && AnySimpleTypeSpecifier(simple))
	{
		bool sign_only = !simple.has_base && !simple.short_count &&
			!simple.long_count &&
			simple.signed_count + simple.unsigned_count == 1;
		if (!sign_combinable || !sign_only ||
		    RemoveTopCv(named)->kind != TK_FUNDAMENTAL)
			throw runtime_error("type-name combined with simple type "
			                    "specifiers");
		named = MakeFundamentalType(SignAdjustedFundamental(
			RemoveTopCv(named)->fundamental,
			simple.unsigned_count > 0));
	}
	if (info.is_typedef &&
	    (info.is_static || info.is_extern || info.is_thread_local ||
	     info.is_constexpr || info.is_inline || info.is_virtual))
		throw runtime_error("typedef combined with another specifier");
	TypePtr base;
	if (info.is_auto)
	{
		if (named || AnySimpleTypeSpecifier(simple))
			throw runtime_error("auto combined with a type specifier");
		// The placeholder resolves from the trailing-return-type
		// (8.3.5p2) or PA24 initializer/return deduction; the marked
		// void stand-in is replaced before any binding is created.
		Type marked;
		marked.kind = TK_FUNDAMENTAL;
		marked.fundamental = FT_VOID;
		marked.is_auto_placeholder = true;
		base = std::make_shared<Type>(marked);
	}
	else
		base = named ? named
			: MakeFundamentalType(CombineSimpleTypeSpecifiers(simple));
	if (complex_form)
	{
		base = host_.MakeGnuComplexType(base);
		if (!base)
			throw OutsideBoundary("complex type");
	}
	info.type = MakeCvQualifiedType(base, is_const, is_volatile);
	return info;
}

// PA33 __decay / PA34 transform family (20.9.7): each transform maps
// the resolved operand type structurally. Dependent operands defer to
// the instantiation-time re-resolution path.
TypePtr TypeBuilder::ResolveTransformSpecifier(const AstSpecifier& spec)
{
	TypePtr operand = ResolveTypeId(*spec.transform_type);
	if (TypeIsDependent(operand))
		throw runtime_error("builtin transform of a dependent type");
	const string& name = spec.spelling;
	if (name == "__decay")
	{
		// 20.9.7.4: references strip, arrays decay to element
		// pointers, functions to function pointers, everything else
		// drops its top cv.
		TypePtr stripped = IsReferenceType(operand)
			? operand->target : operand;
		if (stripped->kind == TK_ARRAY)
			return MakePointerType(stripped->target, false, false);
		if (stripped->kind == TK_FUNCTION)
			return MakePointerType(stripped, false, false);
		return RemoveTopCv(stripped);
	}
	if (name == "__remove_reference_t" || name == "__remove_reference")
		return IsReferenceType(operand) ? operand->target : operand;
	if (name == "__remove_const" || name == "__remove_volatile" ||
	    name == "__remove_cv")
	{
		bool keep_const = name == "__remove_volatile" &&
			operand->is_const;
		bool keep_volatile = name == "__remove_const" &&
			operand->is_volatile;
		return MakeCvQualifiedType(RemoveTopCv(operand), keep_const,
		                           keep_volatile);
	}
	if (name == "__remove_cvref")
	{
		TypePtr stripped = IsReferenceType(operand)
			? operand->target : operand;
		return RemoveTopCv(stripped);
	}
	if (name == "__remove_extent")
		return operand->kind == TK_ARRAY ? operand->target : operand;
	if (name == "__remove_all_extents")
	{
		TypePtr element = operand;
		while (element->kind == TK_ARRAY)
			element = element->target;
		return element;
	}
	if (name == "__remove_pointer")
		return operand->kind == TK_POINTER ? operand->target : operand;
	if (name == "__add_pointer")
	{
		TypePtr stripped = IsReferenceType(operand)
			? operand->target : operand;
		return MakePointerType(stripped, false, false);
	}
	if (name == "__add_lvalue_reference" ||
	    name == "__add_rvalue_reference")
	{
		// 20.9.7.2: non-referenceable types (cv void) pass through;
		// reference collapse applies (&& of & is &).
		if (operand->kind == TK_FUNDAMENTAL &&
		    operand->fundamental == FT_VOID)
			return operand;
		return MakeReferenceType(operand,
		                         name == "__add_rvalue_reference",
		                         true);
	}
	if (name == "__underlying_type")
	{
		if (operand->kind != TK_ENUM)
			throw runtime_error("__underlying_type operand is not an "
			                    "enumeration: " + DescribeType(operand));
		return MakeFundamentalType(operand->named->enum_underlying);
	}
	throw runtime_error("unknown builtin transform: " + name);
}

// Whether the parameter's declarator carries a top-level `...` pack
// marker (`Args... args` / `Args&&...`).
static bool ParameterIsPackExpanded(const AstParameter& parameter)
{
	if (!parameter.declarator)
		return false;
	for (size_t i = 0; i < parameter.declarator->items.size(); i++)
		if (parameter.declarator->items[i].kind == DI_PACK)
			return true;
	return false;
}

void TypeBuilder::BuildParameters(const AstParameterClause& clause,
                                  vector<ParameterInfo>& parameters,
                                  vector<TypePtr>& types,
                                  bool* extra_variadic)
{
	for (size_t i = 0; i < clause.parameters.size(); i++)
	{
		const AstParameter& parameter = clause.parameters[i];
		if (ParameterIsPackExpanded(parameter))
		{
			// PA19: one composed parameter per pack element; PA21: an
			// abstract pattern context keeps the unexpanded element
			// pattern (marked pack_expansion) so partial-specialization
			// function-type patterns deduce structurally.
			vector<ParameterInfo> expanded;
			if (host_.ExpandPackParameter(parameter, expanded))
			{
				for (size_t k = 0; k < expanded.size(); k++)
				{
					host_.OnParameterComposed(expanded[k].name,
					                          expanded[k].type);
					parameters.push_back(expanded[k]);
				}
				continue;
			}
			ParameterInfo pattern;
			if (host_.ComposeAbstractPackParameter(parameter, pattern))
			{
				parameters.push_back(pattern);
				continue;
			}
			// 8.3.5p4: `T...` where T names no parameter pack is an
			// ordinary parameter followed by an ellipsis.
			if (extra_variadic)
			{
				DeclSpecifierInfo pspecs =
					ProcessSpecifiers(parameter.specifiers, false);
				DeclaratorInfo pcomposed = ComposeDeclarator(
					parameter.declarator.get(), pspecs.type);
				ParameterInfo plain;
				plain.type = pcomposed.type;
				if (pcomposed.id && pcomposed.id->IsPlainIdentifier())
					plain.name = pcomposed.id->parts[0].identifier;
				host_.OnParameterComposed(plain.name, plain.type);
				parameters.push_back(plain);
				*extra_variadic = true;
				continue;
			}
			throw runtime_error("parameter pack outside an "
			                    "expandable context");
		}
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
		host_.OnParameterComposed(result.name, composed.type);
		if (parameter.default_arg)
		{
			// 8.3.6: only the `= expression` form; recorded for call
			// sites, analyzed only when an argument is omitted.
			if (parameter.default_arg->kind != INIT_EQ ||
			    !parameter.default_arg->expr)
				throw OutsideBoundary("default argument form");
			result.default_arg = parameter.default_arg->expr.get();
		}
		parameters.push_back(result);
	}
	if (NormalizeVoidParameter(parameters, clause.variadic))
		parameters.clear();
	for (size_t i = 0; i < parameters.size(); i++)
	{
		if (adjust_parameters_)
		{
			// 8.3.5p5: the function type gets the fully adjusted type;
			// the parameter object decays arrays/functions but keeps
			// its declared cv.
			types.push_back(AdjustParameterType(parameters[i].type));
			if (parameters[i].type->kind == TK_ARRAY ||
			    parameters[i].type->kind == TK_FUNCTION)
				parameters[i].type = AdjustParameterType(parameters[i].type);
		}
		else
			types.push_back(parameters[i].type);
		// PA21: the pattern's pack-expansion marker rides the function
		// -type entry.
		if (parameters[i].pack_pattern && !types.back()->pack_expansion)
		{
			Type marked = *types.back();
			marked.pack_expansion = true;
			types.back() = TypePtr(new Type(marked));
		}
	}
}

void TypeBuilder::ApplyDeclaratorSuffix(const AstDeclaratorItem& item,
                                        bool fn_const, bool fn_volatile,
                                        int fn_ref, DeclaratorInfo& out)
{
	switch (item.kind)
	{
	case DI_PARAMS:
	{
		vector<ParameterInfo> parameters;
		vector<TypePtr> types;
		bool extra_variadic = false;
		BuildParameters(*item.params, parameters, types,
		                &extra_variadic);
		if (out.trailing_return)
		{
			// 8.3.5p2: the trailing-return-type replaces the `auto`
			// placeholder return.
			out.type = out.trailing_return;
			out.trailing_return = TypePtr();
		}
		out.type = MakeFunctionType(out.type, types,
		                            item.params->variadic ||
		                                extra_variadic);
		// 8.3.5p6: trailing cv-qualifiers and the ref-qualifier belong
		// to the function type (member functions; the binder rejects
		// them elsewhere).
		out.type = MakeFunctionCvQualifiedType(out.type, fn_const,
		                                       fn_volatile);
		if (fn_ref)
			out.type = MakeRefQualifiedType(out.type, fn_ref);
		out.declares_function = true;
		out.parameters.swap(parameters);
		break;
	}
	case DI_ARRAY:
	{
		host_.CheckArrayElementType(out.type);
		bool bound_known = item.array_bound.get() != 0;
		unsigned long long bound = 0;
		int bound_param = -1;
		if (bound_known &&
		    !host_.AbstractArrayBound(*item.array_bound, bound_param))
			bound = host_.EvaluateArrayBound(*item.array_bound);
		if (bound_param >= 0)
		{
			// PA21 pattern form `T[N]`: the bound is a value-parameter
			// slot deduction fills.
			TypePtr array = MakeArrayType(out.type, false, 0);
			Type marked = *array;
			marked.bound_param = bound_param;
			out.type = TypePtr(new Type(marked));
		}
		else
			out.type = MakeArrayType(out.type, bound_known, bound);
		out.declares_function = false;
		break;
	}
	case DI_TRAILING_RETURN:
		out.trailing_return = ResolveTypeId(*item.trailing_type);
		break;
	case DI_FUNC_QUAL:
		// Exception specifications do not enter the PA11 type model
		// (ref-qualifiers are consumed by the suffix walk). The cheap
		// non-unwinding markings are kept for the PA14 LowIR boundary
		// metadata; PA17 records the virt-specifiers for the binder.
		if (item.qual.kind == FQ_VIRT)
		{
			if (item.qual.spelling == "override")
				out.has_override = true;
			else if (item.qual.spelling == "final")
				out.has_final = true;
			else
				throw OutsideBoundary("virt-specifier");
			break;
		}
		if ((item.qual.kind == FQ_NOEXCEPT && !item.qual.has_expr) ||
		    (item.qual.kind == FQ_THROW && item.qual.throw_types.empty()))
			out.noexcept_simple = true;
		else if (item.qual.kind == FQ_THROW)
			// PA36 15.4: keep the dynamic spec's types as typed state
			// (adjusted per 15.4p2) for the host filter-region lowering.
			for (size_t i = 0; i < item.qual.throw_types.size(); i++)
			{
				TypePtr spec = ResolveTypeId(*item.qual.throw_types[i]);
				if (spec->kind == TK_ARRAY)
					spec = MakePointerType(spec->target, false, false);
				else if (spec->kind == TK_FUNCTION)
					spec = MakePointerType(spec, false, false);
				out.throw_spec_types.push_back(spec);
			}
		else if (item.qual.kind == FQ_NOEXCEPT && item.qual.has_expr &&
		         item.qual.expr)
		{
			// PA36 15.4p1: noexcept(constant-expression) evaluated
			// true is a non-throwing specification like the bare form.
			bool spec_value = false;
			if (host_.EvaluateNoexceptSpec(*item.qual.expr,
			                               spec_value) &&
			    spec_value)
				out.noexcept_simple = true;
		}
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
void TypeBuilder::ComposeItems(const vector<AstDeclaratorItem>& items_in,
                               bool collapsible, DeclaratorInfo& out)
{
	// PA19: the `...` pack marker is expansion structure, not type
	// structure - composing the pattern (one element's type) ignores
	// it. AstDeclaratorItem owns unique_ptrs, so the filtered view
	// indexes the original items.
	vector<const AstDeclaratorItem*> filtered;
	bool has_pack = false;
	for (size_t i = 0; i < items_in.size(); i++)
		if (items_in[i].kind == DI_PACK)
			has_pack = true;
	if (has_pack)
	{
		for (size_t i = 0; i < items_in.size(); i++)
			if (items_in[i].kind != DI_PACK)
				filtered.push_back(&items_in[i]);
		ComposeItemPtrs(filtered, collapsible, out);
		return;
	}
	vector<const AstDeclaratorItem*> all;
	for (size_t i = 0; i < items_in.size(); i++)
		all.push_back(&items_in[i]);
	ComposeItemPtrs(all, collapsible, out);
}

void TypeBuilder::ComposeItemPtrs(
	const vector<const AstDeclaratorItem*>& item_ptrs, bool collapsible,
	DeclaratorInfo& out)
{
	struct ItemsView
	{
		const vector<const AstDeclaratorItem*>& ptrs;
		const AstDeclaratorItem& operator[](size_t i) const
		{
			return *ptrs[i];
		}
		size_t size() const { return ptrs.size(); }
	};
	ItemsView items = {item_ptrs};
	size_t prefix_end = 0;
	while (prefix_end < items.size() &&
	       (items[prefix_end].kind == DI_PTR ||
	        items[prefix_end].kind == DI_MEMBER_PTR ||
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
		if (item.kind == DI_MEMBER_PTR)
		{
			TypePtr cls = host_.ResolveTypeName(item.name);
			// PA26: a dependent qualifier (`T::*` in a pattern) keeps
			// the parameter/specialization entity; deduction and
			// substitution resolve it per instantiation.
			if (cls->kind != TK_CLASS && cls->kind != TK_TYPE_PARAM &&
			    cls->kind != TK_TEMPLATE_SPEC)
				throw runtime_error("member-pointer qualifier does not "
				                    "name a class");
			out.type = MakeMemberPointerType(cls->named, out.type,
			                                 ptr_const, ptr_volatile);
		}
		else if (item.token == OP_STAR)
			out.type = MakePointerType(out.type, ptr_const, ptr_volatile);
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
	bool fn_const = false;
	bool fn_volatile = false;
	int fn_ref = 0;
	for (size_t i = items.size(); i > suffix_begin; i--)
	{
		const AstDeclaratorItem& item = items[i - 1];
		if (item.kind == DI_CV)
		{
			// Trailing cv-qualifiers apply to the parameter clause they
			// follow (8.3.5p6), composed right-to-left just below.
			if (item.token == KW_CONST)
				fn_const = true;
			else
				fn_volatile = true;
			continue;
		}
		if (item.kind == DI_FUNC_QUAL && item.qual.kind == FQ_VIRT &&
		    (item.qual.spelling == "&" || item.qual.spelling == "&&"))
		{
			// 8.3.5p6: the ref-qualifier of the parameter clause it
			// follows.
			fn_ref = item.qual.spelling == "&" ? 1 : 2;
			continue;
		}
		ApplyDeclaratorSuffix(item, fn_const, fn_volatile, fn_ref, out);
		fn_const = false;
		fn_volatile = false;
		fn_ref = 0;
		collapsible = false;
	}
	if (fn_const || fn_volatile)
		throw runtime_error("cv-qualifier without a parameter clause");
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
	{
		ComposeItems(declarator->items, true, info);
		info.asm_label = declarator->asm_label;
	}
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
