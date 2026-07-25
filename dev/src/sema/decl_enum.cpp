#include "sema/decl_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

// PA11 7.2 enumerations: entity declaration/redeclaration agreement,
// enumerator value computation with the unscoped widening ladder
// (7.2p6), and the scoped member-scope model. Split from
// decl_binder.cpp, which keeps the declaration dispatch and class
// layout.

using std::runtime_error;
using std::to_string;
using std::vector;

namespace {

// True when the two typed values denote the same integer: the stored
// 64 bits agree and so does their sign interpretation.
bool SameIntegerValue(const ConstValue& a, const ConstValue& b)
{
	bool a_negative = IsSignedIntegralFundamental(a.type) &&
		(long long)a.bits < 0;
	bool b_negative = IsSignedIntegralFundamental(b.type) &&
		(long long)b.bits < 0;
	return a.bits == b.bits && a_negative == b_negative;
}

// 7.2p5: the implicit value of an enumerator without an initializer is
// one more than the previous enumerator, and must be representable in
// the underlying type.
ConstValue SuccessorValue(const ConstValue& value, EFundamentalType type)
{
	bool is_signed = IsSignedIntegralFundamental(type);
	if (value.bits == (is_signed ? 0x7fffffffffffffffull : ~0ull))
		throw runtime_error("enumerator value overflows the underlying "
		                    "type");
	ConstValue next = ConvertConstValue(ConstValue(type, value.bits + 1),
	                                    type);
	if (next.bits != value.bits + 1)
		throw runtime_error("enumerator value overflows the underlying "
		                    "type");
	return next;
}

}  // namespace

TypePtr DeclBinder::DeclareEnumEntity(const AstDecl& decl,
                                      const string& name, bool scoped,
                                      const TypePtr& underlying)
{
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		// 7.2p2-p4: every redeclaration must agree on scoped-ness and
		// underlying type.
		if (existing->kind != SB_TYPE ||
		    existing->type->kind != TK_ENUM)
			throw runtime_error(name + " redeclared as an enumeration");
		const NamedTypeInfo* info = existing->type->named;
		if (info->is_scoped != scoped ||
		    info->enum_underlying != underlying->fundamental)
			throw runtime_error("conflicting redeclaration of enum " +
			                    name);
		return existing->type;
	}
	NamedTypeInfo* info = model_.CreateNamedTypeInfo(
		TypeDisplayName(scoped ? "enum class" : "enum", name), current_,
		name);
	info->complete = true;
	// 7.1.3p9: an unnamed enumeration's placeholder is display-only.
	info->unnamed = decl.name.empty();
	info->is_scoped = scoped;
	info->enum_underlying = underlying->fundamental;
	info->size = TypeSize(underlying);
	info->alignment = TypeAlignment(underlying);
	TypePtr type = MakeNamedType(TK_ENUM, info);
	if (!decl.name.empty())
	{
		ScopeBinding binding;
		binding.kind = SB_TYPE;
		binding.access = current_access_;
		binding.name = name;
		binding.type = type;
		AddBinding(*current_, binding);
	}
	// Scoped enumerations own a member scope from their first
	// declaration (the fixtures print it even for opaque declarations).
	if (scoped)
		model_.SetMemberScope(
			info, model_.CreateScope(SCOPE_ENUM, name, current_));
	return type;
}

void DeclBinder::BindEnumerators(const AstDecl& decl,
                                 const TypePtr& enum_type)
{
	EFundamentalType underlying = enum_type->named->enum_underlying;
	// 7.2p6: an unscoped enumeration without a fixed base takes an
	// underlying type wide enough for every enumerator; the default
	// int widens through the host ladder when a value needs it.
	bool can_widen = !enum_type->named->is_scoped && !decl.has_enum_base;
	Scope* target = enum_type->named->is_scoped
		? model_.MemberScope(enum_type->named) : current_;
	Scope* saved = current_;
	// 7.2p10: earlier enumerators are in scope inside the list.
	current_ = target;
	vector<ConstValue> raws;
	bool have_prev = false;
	ConstValue prev(underlying, 0);
	for (size_t i = 0; i < decl.enumerators.size(); i++)
	{
		const AstEnumerator& enumerator = decl.enumerators[i];
		// 7.2p5: the successor computes only when this enumerator has
		// no initializer (a predecessor at the type maximum followed
		// by an initialized enumerator is fine).
		ConstValue raw(underlying, 0);
		if (enumerator.value)
			raw = EvaluateConstExpr(*enumerator.value, *this);
		else if (have_prev)
			raw = SuccessorValue(prev, underlying);
		raws.push_back(raw);
		ConstValue value = ConvertConstValue(raw, underlying);
		if (!SameIntegerValue(raw, value))
		{
			// Widen to the first host type representing every
			// enumerator seen so far; silently wrapping would dump a
			// value the program never wrote.
			static const EFundamentalType kLadder[] = {
				FT_INT, FT_UNSIGNED_INT, FT_LONG_INT,
				FT_UNSIGNED_LONG_INT
			};
			EFundamentalType widened = FT_INT;
			bool found = false;
			for (size_t c = 0; c < 4 && !found; c++)
			{
				bool fits_all = true;
				for (size_t r = 0; r < raws.size() && fits_all; r++)
					fits_all = SameIntegerValue(
						raws[r],
						ConvertConstValue(raws[r], kLadder[c]));
				if (fits_all)
				{
					widened = kLadder[c];
					found = true;
				}
			}
			if (!can_widen || !found)
				throw runtime_error(enumerator.name + " is not "
				                    "representable in the underlying type");
			underlying = widened;
			NamedTypeInfo* info = model_.MutableInfo(enum_type->named);
			info->enum_underlying = underlying;
			TypePtr ut = MakeFundamentalType(underlying);
			info->size = TypeSize(ut);
			info->alignment = TypeAlignment(ut);
			for (size_t r = 0; r + 1 < raws.size(); r++)
			{
				ScopeBinding* earlier = FindOwnBinding(
					*target, decl.enumerators[r].name);
				if (earlier)
					earlier->value = ConvertConstValue(raws[r],
					                                   underlying);
			}
			value = ConvertConstValue(raw, underlying);
		}
		ScopeBinding binding;
		binding.kind = SB_ENUMERATOR;
		binding.access = current_access_;
		binding.name = enumerator.name;
		binding.type = enum_type;
		binding.has_value = true;
		binding.value = value;
		AddBinding(*target, binding);
		prev = value;
		have_prev = true;
	}
	current_ = saved;
}

TypePtr DeclBinder::BindEnum(const AstDecl& decl, bool elaborated)
{
	bool scoped = decl.has_enum_key;
	string name = decl.name;
	if (name.empty())
	{
		if (scoped || !decl.enum_body)
			throw runtime_error("anonymous enumeration form");
		name = "__anonymous_enum" + to_string(++anonymous_enums_);
	}
	if (elaborated && !decl.enum_body)
	{
		// 7.1.6.3: the elaborated-enum-specifier grammar has no
		// enum-base and no class-key.
		if (scoped || decl.has_enum_base)
			throw runtime_error("enum-base or enum-key in elaborated "
			                    "enum specifier");
		// 3.4.4p2 / 7.1.6.3p3: the elaborated form `enum E` used as a
		// type specifier only refers to an already-declared
		// enumeration; it declares nothing.
		const ScopeBinding* found =
			UnqualifiedLookup(current_, name, SLF_SCOPE_NAMES);
		if (!found || found->kind != SB_TYPE ||
		    found->type->kind != TK_ENUM)
			throw runtime_error(name +
			                    " does not name an enumeration");
		return found->type;
	}
	TypePtr underlying;
	if (decl.has_enum_base)
	{
		underlying = RemoveTopCv(builder_.ResolveTypeId(*decl.type));
		if (!IsIntegralType(underlying))
			throw runtime_error("enum underlying type must be integral");
	}
	else if (scoped || decl.enum_body)
		// 7.2p5 for scoped enumerations; for unscoped definitions the
		// PA11 model fixes int rather than computing a value-dependent
		// type.
		underlying = MakeFundamentalType(FT_INT);
	else
		// 7.2p2: an opaque unscoped declaration requires a fixed base.
		throw runtime_error("opaque unscoped enum declaration");

	TypePtr type = DeclareEnumEntity(decl, name, scoped, underlying);
	if (decl.enum_body)
	{
		NamedTypeInfo* info = model_.MutableInfo(type->named);
		if (info->is_defined)
			throw runtime_error("redefinition of enum " + name);
		info->is_defined = true;
		BindEnumerators(decl, type);
	}
	return type;
}

