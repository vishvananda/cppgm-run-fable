#include "sema/const_eval.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "numeric_literals.h"
#include "sema/const_expr.h"

using std::runtime_error;

// Storage access, conversions, and the body registry of the PA20
// constant-evaluation engine. Expression and statement evaluation
// live in const_eval_expr.cpp / const_eval_stmt.cpp.

namespace {

const long long kStepLimit = 1 << 22;

runtime_error NotConstant(const string& what)
{
	return runtime_error(what + " is not a constant expression");
}

int ScalarWidth(EFundamentalType type)
{
	switch (type)
	{
	case FT_BOOL:
	case FT_CHAR:
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
		return 1;
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T:
		return 2;
	case FT_INT:
	case FT_UNSIGNED_INT:
	case FT_WCHAR_T:
	case FT_CHAR32_T:
	case FT_FLOAT:
		return 4;
	case FT_LONG_DOUBLE:
		return 16;
	default:
		return 8;
	}
}

// The scalar classification of a (top-cv-stripped) type: integral or
// enum -> the underlying fundamental; floating -> the float kind;
// pointerish kinds report through `is_pointer`.
EFundamentalType ScalarKind(const TypePtr& type, bool& is_pointer)
{
	TypePtr bare = RemoveTopCv(type);
	is_pointer = false;
	if (bare->kind == TK_POINTER || bare->kind == TK_MEMBER_POINTER ||
	    (bare->kind == TK_FUNDAMENTAL &&
	     bare->fundamental == FT_NULLPTR_T))
	{
		is_pointer = true;
		return FT_NULLPTR_T;
	}
	if (bare->kind == TK_ENUM)
		return bare->named->enum_underlying;
	if (bare->kind == TK_FUNDAMENTAL)
		return bare->fundamental;
	throw NotConstant("non-scalar value");
}

}  // namespace

// --- registry ---------------------------------------------------------------

ConstBodyRegistry::ConstBodyRegistry(SemUnit& unit)
	: unit_(unit), items_seen_(0), deferred_seen_(0), synthesized_seen_(0)
{
}

void ConstBodyRegistry::Index(const SemNode& node)
{
	if (node.kind != SN_FUNCTION_DEFINITION)
		return;
	if (node.fn_spec)
		by_spec_[node.fn_spec] = &node;
	if (node.entity_scope)
		by_entity_[pair<const void*, string>(
			node.entity_scope, node.entity_name)].push_back(&node);
	by_name_[node.name].push_back(&node);
}

void ConstBodyRegistry::Refresh()
{
	for (; items_seen_ < unit_.items.size(); items_seen_++)
		Index(*unit_.items[items_seen_]);
	for (; deferred_seen_ < unit_.deferred.size(); deferred_seen_++)
		Index(*unit_.deferred[deferred_seen_]);
	for (; synthesized_seen_ < unit_.synthesized.size();
	     synthesized_seen_++)
		Index(*unit_.synthesized[synthesized_seen_]);
}

void ConstBodyRegistry::Invalidate()
{
	items_seen_ = 0;
	deferred_seen_ = 0;
	synthesized_seen_ = 0;
	by_spec_.clear();
	by_entity_.clear();
	by_name_.clear();
}

const SemNode* ConstBodyRegistry::Find(const SemNode& callee)
{
	Refresh();
	if (callee.fn_spec)
	{
		map<const void*, const SemNode*>::const_iterator found =
			by_spec_.find(callee.fn_spec);
		return found == by_spec_.end() ? 0 : found->second;
	}
	// Primary: the declaring scope + declared name identity, then the
	// display name (synthesized helpers without a stamped scope).
	if (callee.entity_scope)
	{
		map<pair<const void*, string>,
		    vector<const SemNode*>>::const_iterator entity =
			by_entity_.find(pair<const void*, string>(
				callee.entity_scope, callee.entity_name));
		if (entity != by_entity_.end())
			for (size_t i = 0; i < entity->second.size(); i++)
				if (TypeEquals(entity->second[i]->type, callee.type))
					return entity->second[i];
	}
	map<string, vector<const SemNode*>>::const_iterator found =
		by_name_.find(callee.name);
	if (found == by_name_.end())
		return 0;
	const vector<const SemNode*>& candidates = found->second;
	const SemNode* named_match = 0;
	for (size_t i = 0; i < candidates.size(); i++)
	{
		const SemNode& definition = *candidates[i];
		if (!TypeEquals(definition.type, callee.type))
			continue;
		if (!definition.fn_spec)
			return &definition;
		if (!named_match)
			named_match = &definition;
	}
	return named_match;
}

// --- engine state -----------------------------------------------------------

void ConstEvalEngine::InvalidateBodies()
{
	registry_.Invalidate();
}

ConstEvalEngine::ConstEvalEngine(SemUnit& unit)
	: unit_(unit), registry_(unit), depth_(0), steps_(0)
{
}

ConstEvalEngine::Frame* ConstEvalEngine::CurrentFrame()
{
	return frames_.empty() ? 0 : frames_.back();
}

void ConstEvalEngine::Step()
{
	if (++steps_ > kStepLimit)
		throw NotConstant("evaluation exceeding the step limit");
}

void ConstEvalEngine::StoreObject(const Scope* scope, const string& name,
                                  const ConstObjectPtr& object)
{
	store_[pair<const void*, string>(scope, name)] = object;
}

ConstObjectPtr ConstEvalEngine::FindObject(const Scope* scope,
                                           const string& name) const
{
	map<pair<const void*, string>, ConstObjectPtr>::const_iterator found =
		store_.find(pair<const void*, string>(scope, name));
	return found == store_.end() ? ConstObjectPtr() : found->second;
}

void ConstEvalEngine::StoreScalarObject(const Scope* scope,
                                        const string& name,
                                        const TypePtr& type,
                                        const EvalValue& value)
{
	ConstObjectPtr object = Allocate(type);
	WriteScalar(*object, 0, RemoveTopCv(type), value);
	StoreObject(scope, name, object);
}

// --- storage ----------------------------------------------------------------

ConstObjectPtr ConstEvalEngine::Allocate(const TypePtr& type)
{
	ConstObjectPtr object(new ConstObject());
	object->type = type;
	object->bytes.resize(TypeSize(RemoveTopCv(type)), 0);
	return object;
}

const ConstObject& ConstEvalEngine::Readable(
	const ConstPointer& address) const
{
	if (!address.object)
		throw NotConstant("read through a non-constant address");
	return *address.object;
}

ConstObject& ConstEvalEngine::Writable(const ConstPointer& address)
{
	if (!address.object)
		throw NotConstant("write through a non-constant address");
	return *address.object;
}

EvalValue ConstEvalEngine::ReadScalar(const ConstObject& object,
                                      unsigned long long offset,
                                      const TypePtr& type)
{
	bool is_pointer = false;
	EFundamentalType kind = ScalarKind(type, is_pointer);
	EvalValue value;
	value.type = RemoveTopCv(type);
	if (is_pointer)
	{
		value.kind = EvalValue::EV_PTR;
		map<unsigned long long, ConstPointer>::const_iterator slot =
			object.ptr_slots.find(offset);
		if (slot != object.ptr_slots.end())
			value.ptr = slot->second;
		// No recorded slot: only an all-zero image (null) is a value.
		else
			for (size_t i = 0; i < 8; i++)
				if (offset + i < object.bytes.size() &&
				    object.bytes[offset + i])
					throw NotConstant("indeterminate pointer read");
		return value;
	}
	size_t width = ScalarWidth(kind);
	if (offset + width > object.bytes.size())
		throw NotConstant("out-of-range read");
	const unsigned char* at = &object.bytes[offset];
	if (IsFloatingFundamental(kind))
	{
		value.kind = EvalValue::EV_FLOAT;
		if (kind == FT_FLOAT)
		{
			float f;
			std::memcpy(&f, at, sizeof(f));
			value.fval = f;
		}
		else if (kind == FT_DOUBLE)
		{
			double d;
			std::memcpy(&d, at, sizeof(d));
			value.fval = d;
		}
		else
		{
			long double ld;
			std::memcpy(&ld, at, sizeof(ld));
			value.fval = ld;
		}
		return value;
	}
	unsigned long long bits = 0;
	unsigned long long high = 0;
	for (size_t i = 0; i < width; i++)
	{
		if (i < 8)
			bits |= (unsigned long long)at[i] << (8 * i);
		else
			high |= (unsigned long long)at[i] << (8 * (i - 8));
	}
	if (IsSignedIntegralFundamental(kind) && width < 8 &&
	    (bits & (1ull << (8 * width - 1))))
		bits |= ~((1ull << (8 * width)) - 1);
	value.kind = EvalValue::EV_INT;
	value.ival = ConstValue(kind, bits, high);
	return value;
}

void ConstEvalEngine::WriteScalar(ConstObject& object,
                                  unsigned long long offset,
                                  const TypePtr& type,
                                  const EvalValue& value)
{
	bool is_pointer = false;
	EFundamentalType kind = ScalarKind(type, is_pointer);
	if (is_pointer)
	{
		if (value.kind != EvalValue::EV_PTR)
			throw NotConstant("pointer store of a non-pointer value");
		if (offset + 8 > object.bytes.size())
			throw NotConstant("out-of-range write");
		for (size_t i = 0; i < 8; i++)
			object.bytes[offset + i] = 0;
		if (value.ptr.IsNull())
			object.ptr_slots.erase(offset);
		else
			object.ptr_slots[offset] = value.ptr;
		return;
	}
	size_t width = ScalarWidth(kind);
	if (offset + width > object.bytes.size())
		throw NotConstant("out-of-range write");
	unsigned char* at = &object.bytes[offset];
	if (IsFloatingFundamental(kind))
	{
		if (value.kind != EvalValue::EV_FLOAT)
			throw NotConstant("floating store of a non-float value");
		if (kind == FT_FLOAT)
		{
			float f = (float)value.fval;
			std::memcpy(at, &f, sizeof(f));
		}
		else if (kind == FT_DOUBLE)
		{
			double d = (double)value.fval;
			std::memcpy(at, &d, sizeof(d));
		}
		else
		{
			long double ld = value.fval;
			std::memset(at, 0, 16);
			std::memcpy(at, &ld, sizeof(ld));
		}
		return;
	}
	if (value.kind != EvalValue::EV_INT)
		throw NotConstant("integral store of a non-integral value");
	for (size_t i = 0; i < width; i++)
		at[i] = (unsigned char)(i < 8 ? value.ival.bits >> (8 * i)
		                              : value.ival.high >> (8 * (i - 8)));
}

void ConstEvalEngine::WriteValue(const ConstPointer& dest,
                                 const TypePtr& type,
                                 const EvalValue& value)
{
	WriteScalar(Writable(dest), dest.offset, type, value);
}

EvalValue ConstEvalEngine::ReadThrough(const ConstPointer& address,
                                       const TypePtr& type)
{
	return ReadScalar(Readable(address), address.offset, type);
}

void ConstEvalEngine::CopyBytes(const ConstPointer& dest,
                                const ConstPointer& src,
                                unsigned long long size)
{
	const ConstObject& from = Readable(src);
	ConstObject& to = Writable(dest);
	if (src.offset + size > from.bytes.size() ||
	    dest.offset + size > to.bytes.size())
		throw NotConstant("out-of-range object copy");
	// Self-copies at identical positions are no-ops; overlapping cases
	// do not occur in the evaluated subset.
	vector<unsigned char> staged(from.bytes.begin() + src.offset,
	                             from.bytes.begin() + src.offset + size);
	map<unsigned long long, ConstPointer> slots;
	for (map<unsigned long long, ConstPointer>::const_iterator it =
	         from.ptr_slots.lower_bound(src.offset);
	     it != from.ptr_slots.end() && it->first < src.offset + size; ++it)
		slots[it->first - src.offset] = it->second;
	for (unsigned long long i = 0; i < size; i++)
		to.bytes[dest.offset + i] = staged[i];
	for (map<unsigned long long, ConstPointer>::iterator it =
	         to.ptr_slots.lower_bound(dest.offset);
	     it != to.ptr_slots.end() && it->first < dest.offset + size;)
		to.ptr_slots.erase(it++);
	for (map<unsigned long long, ConstPointer>::const_iterator it =
	         slots.begin();
	     it != slots.end(); ++it)
		to.ptr_slots[dest.offset + it->first] = it->second;
}

// --- conversions ------------------------------------------------------------

EvalValue ConstEvalEngine::Convert(const EvalValue& value,
                                   const TypePtr& dest)
{
	TypePtr bare = RemoveTopCv(dest);
	if (IsReferenceType(bare))
		bare = RemoveTopCv(bare->target);
	EvalValue result;
	result.type = bare;
	if (bare->kind == TK_FUNDAMENTAL && bare->fundamental == FT_BOOL)
	{
		bool truth = value.kind == EvalValue::EV_INT
			? value.ival.bits != 0
			: value.kind == EvalValue::EV_FLOAT
				? value.fval != 0
				: !value.ptr.IsNull();
		result.kind = EvalValue::EV_INT;
		result.ival = ConstValue(FT_BOOL, truth ? 1 : 0);
		return result;
	}
	bool is_pointer = false;
	EFundamentalType kind = ScalarKind(bare, is_pointer);
	if (is_pointer)
	{
		if (value.kind != EvalValue::EV_PTR)
		{
			if (value.kind == EvalValue::EV_INT && value.ival.bits == 0)
			{
				result.kind = EvalValue::EV_PTR;
				return result;
			}
			throw NotConstant("pointer conversion of a non-pointer");
		}
		result.kind = EvalValue::EV_PTR;
		result.ptr = value.ptr;
		return result;
	}
	if (IsFloatingFundamental(kind))
	{
		result.kind = EvalValue::EV_FLOAT;
		if (value.kind == EvalValue::EV_FLOAT)
			result.fval = value.fval;
		else if (value.kind == EvalValue::EV_INT)
			result.fval = IsSignedIntegralFundamental(value.ival.type)
				? (long double)(long long)value.ival.bits
				: (long double)value.ival.bits;
		else
			throw NotConstant("floating conversion of a pointer");
		// Round through the destination precision.
		if (kind == FT_FLOAT)
			result.fval = (float)result.fval;
		else if (kind == FT_DOUBLE)
			result.fval = (double)result.fval;
		return result;
	}
	result.kind = EvalValue::EV_INT;
	if (value.kind == EvalValue::EV_INT)
		result.ival = ConvertConstValue(value.ival, kind);
	else if (value.kind == EvalValue::EV_FLOAT)
		result.ival = ConvertConstValue(
			ConstValue(FT_LONG_LONG_INT,
			           (unsigned long long)(long long)value.fval),
			kind);
	else
		throw NotConstant("integral conversion of a pointer");
	return result;
}

// --- entry points -----------------------------------------------------------

bool ConstEvalEngine::EvaluateCondition(const SemNode& expr)
{
	steps_ = 0;
	return EvaluateBool(expr);
}

bool ConstEvalEngine::EvaluateBool(const SemNode& expr)
{
	EvalValue value = EvalRead(expr);
	switch (value.kind)
	{
	case EvalValue::EV_INT:
		return value.ival.bits != 0;
	case EvalValue::EV_FLOAT:
		return value.fval != 0;
	default:
		return !value.ptr.IsNull();
	}
}

ConstValue ConstEvalEngine::EvaluateIntegral(const SemNode& expr)
{
	steps_ = 0;
	EvalValue value = EvalRead(expr);
	if (value.kind != EvalValue::EV_INT)
		throw NotConstant("non-integral constant");
	return value.ival;
}

EvalValue ConstEvalEngine::EvaluateScalar(const SemNode& expr)
{
	steps_ = 0;
	return EvalRead(expr);
}

ConstObjectPtr ConstEvalEngine::EvaluateVariableInit(const SemNode& item,
                                                     const TypePtr& type,
                                                     const Scope* scope,
                                                     const string& name)
{
	steps_ = 0;
	TypePtr bare = RemoveTopCv(type);
	if (IsReferenceType(bare))
	{
		// A constexpr reference binds a constant address: store it as
		// a one-pointer image.
		if (item.children.empty())
			throw NotConstant("reference without an initializer");
		ConstObjectPtr object(new ConstObject());
		object->type = type;
		object->bytes.resize(8, 0);
		ConstPointer bound = EvalLValue(*item.children[0]);
		EvalValue value;
		value.kind = EvalValue::EV_PTR;
		value.ptr = bound;
		WriteScalar(*object, 0, MakePointerType(bare->target, false, false),
		            value);
		StoreObject(scope, name, object);
		return object;
	}
	ConstObjectPtr object = Allocate(bare);
	ConstPointer dest;
	dest.object = object;
	// The initializer actions address the object by name (constructor
	// calls, aggregate member stores): register first.
	StoreObject(scope, name, object);
	try
	{
		for (size_t i = 0; i < item.children.size(); i++)
			ApplyInitAction(*item.children[i], dest, bare);
	}
	catch (...)
	{
		store_.erase(pair<const void*, string>(scope, name));
		throw;
	}
	return object;
}

void ConstEvalEngine::StampScalar(SemNode& node, const ConstValue& value)
{
	node.has_value = true;
	node.value = value;
}

// --- floating tokens ----------------------------------------------------------

bool DecodeFloatLiteral(const string& spelling, long double& out)
{
	PostToken token = AnalyzePPNumber(spelling);
	if (token.kind != PTK_LITERAL)
		return false;
	// A folded token may render integrally ("3"); the numeric value is
	// still the float constant.
	if (IsIntegralFundamental(token.type))
	{
		unsigned long long bits = LittleEndianValue(token.data);
		out = IsSignedIntegralFundamental(token.type)
			? (long double)(long long)bits : (long double)bits;
		return true;
	}
	if (token.type == FT_FLOAT)
	{
		float f;
		std::memcpy(&f, token.data.data(), sizeof(f));
		out = f;
		return true;
	}
	if (token.type == FT_DOUBLE)
	{
		double d;
		std::memcpy(&d, token.data.data(), sizeof(d));
		out = d;
		return true;
	}
	if (token.type == FT_LONG_DOUBLE)
	{
		long double ld;
		std::memcpy(&ld, token.data.data(), sizeof(ld));
		out = ld;
		return true;
	}
	return false;
}

string RenderFloatConstant(long double value, EFundamentalType type)
{
	char buffer[64];
	for (int precision = 1; precision <= 21; precision++)
	{
		std::snprintf(buffer, sizeof(buffer), "%.*Lg", precision, value);
		long double parsed = std::strtold(buffer, 0);
		bool exact = type == FT_FLOAT
			? (float)parsed == (float)value
			: type == FT_DOUBLE ? (double)parsed == (double)value
			                    : parsed == value;
		if (exact)
			break;
	}
	return buffer;
}

// --- noexcept fact walk -------------------------------------------------------

bool SemTreeMayThrow(const SemNode& node)
{
	if (node.kind == SN_THROW)
		return true;
	if (node.kind == SN_CALL_EXPRESSION && !node.children.empty())
	{
		// An indirect call has no known specification; a direct call
		// reads the declared (or implicit-member) specification, not
		// the derived body fact (5.3.7).
		if (node.children[0]->kind != SN_CALLEE)
			return true;
		if (!node.children[0]->noexcept_decl)
			return true;
	}
	for (size_t i = 0; i < node.children.size(); i++)
		if (SemTreeMayThrow(*node.children[i]))
			return true;
	return false;
}
