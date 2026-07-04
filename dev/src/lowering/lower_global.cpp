#include "lowering/lower_program.h"

#include <cstring>
#include <stdexcept>

#include "lowering/lower_const.h"
#include "lowering/lower_types.h"
#include "sema/const_eval.h"
#include "sema/const_expr.h"

using std::runtime_error;
using std::to_string;

// PA14/PA20 global-definition rendering: metadata, scalar and
// structured-array initializers, and the PA20 evaluated-image
// emission of object-valued constant definitions. The registry and
// program assembly live in lower_unit.cpp.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

// The fundamental value space of an integral or enumeration type.
EFundamentalType ValueFund(const TypePtr& type)
{
	if (type->kind == TK_ENUM)
		return type->named->enum_underlying;
	return type->fundamental;
}

// Floating global initializers keep the literal spelling; the type
// suffix follows the destination type.
string FloatInitToken(const TypePtr& type, const LowerConst& value)
{
	string token = value.float_token;
	char last = token.empty() ? 0 : token[token.size() - 1];
	bool suffixed = last == 'f' || last == 'F' || last == 'l' ||
		last == 'L';
	if (!suffixed && type->fundamental == FT_FLOAT)
		token += "f";
	else if (!suffixed && type->fundamental == FT_LONG_DOUBLE)
		token += "L";
	return token;
}

}  // namespace

// --- rendering -------------------------------------------------------------

// Thread-local objects expose their access wrapper (the backend's
// TLS entry point).
void LowerProgram::AppendTlsWrapperDeclares(vector<string>& declares)
{
	for (size_t i = 0; i < globals_.size(); i++)
	{
		const LowGlobalInfo& info = globals_[i];
		if (!info.is_thread_local || (!info.defined && !info.used))
			continue;
		string object = info.object_name;
		if (object.compare(0, 2, "_Z") == 0)
			object = "_ZTW" + object.substr(2);
		string meta = info.internal ? "binding=internal"
		                            : "binding=strong";
		if (!object.empty())
			meta += ", object=" + object;
		declares.push_back(
			"declare function @" + info.low_name +
			"__tls_wrapper() -> ptr [" + meta +
			", tls_for=@" + info.low_name + "]");
	}
}


string LowerProgram::GlobalMetadata(const LowGlobalInfo& info) const
{
	string meta = info.weak ? "binding=weak"
		: info.internal ? "binding=internal" : "binding=strong";
	if (!info.object_name.empty())
		meta += ", object=" + info.object_name;
	if (info.is_thread_local)
		meta += ", storage=thread_local";
	return " [" + meta + "]";
}

string LowerProgram::RenderAddress(const LowerConst& value)
{
	string symbol;
	if (value.kind == LC_STRING)
		symbol = StringLiteralRef(*value.string_node);
	else if (value.entity_type->kind == TK_FUNCTION)
		symbol = FunctionRef(value.entity_scope, value.entity_name,
		                     value.entity_type);
	else
		symbol = GlobalRef(value.entity_scope, value.entity_name);
	string text = "addr " + symbol;
	if (value.kind == LC_ADDR && value.offset)
	{
		if (value.offset < 0)
			throw OutsideBoundary("negative address offset");
		text += " + " + to_string(value.offset);
	}
	return text;
}

// One structured data item; `is_zero_item` marks null pointers, which
// spell as zero fill.
string LowerProgram::RenderConstItem(const LowerConst& value,
                                     const TypePtr& type,
                                     bool& is_zero_item)
{
	is_zero_item = false;
	if (value.kind == LC_NULL)
	{
		is_zero_item = true;
		return "zero " + to_string(TypeSize(type));
	}
	if (value.kind == LC_INT)
		return LowerValueType(type) + " " +
			RenderConstValue(ConvertConstValue(value.ival,
			                                   ValueFund(type)));
	if (value.kind == LC_FLOAT)
		return LowerValueType(type) + " " + FloatInitToken(type, value);
	if (value.offset)
		throw OutsideBoundary("structured address item offset");
	return "ptr " + RenderAddress(value);
}

string LowerProgram::RenderScalarInit(const LowGlobalInfo& info)
{
	if (!info.node || info.node->children.empty() || info.dynamic_init)
		return "zero";
	LowerConst value;
	if (!EvaluateLowerConst(*info.node->children[0], value))
		throw OutsideBoundary("non-constant global initializer");
	switch (value.kind)
	{
	case LC_INT:
		return RenderConstValue(ConvertConstValue(value.ival,
		                                          ValueFund(info.type)));
	case LC_FLOAT:
	{
		// Scalar float initializers render numerically (the reference
		// presentation); structured array items keep source tokens.
		long double parsed = 0;
		if (!DecodeFloatLiteral(value.float_token, parsed))
			return FloatInitToken(info.type, value);
		EFundamentalType fund = info.type->fundamental;
		if (fund == FT_FLOAT)
			parsed = (float)parsed;
		else if (fund == FT_DOUBLE)
			parsed = (double)parsed;
		return RenderFloatConstant(parsed, fund);
	}
	case LC_NULL:
		// A null-pointer initializer spells the immediate zero.
		return "0";
	case LC_ADDR:
	case LC_STRING:
		return RenderAddress(value);
	}
	throw OutsideBoundary("global initializer");
}

string LowerProgram::RenderArrayItems(const LowGlobalInfo& info)
{
	const TypePtr& array = info.type;
	TypePtr element = RemoveTopCv(array->target);
	unsigned long long element_size = TypeSize(element);
	const SemNode* braced = 0;
	if (info.node && !info.node->children.empty())
	{
		braced = info.node->children[0].get();
		if (braced->kind != SN_BRACED_INIT_LIST)
			throw OutsideBoundary("global array initializer form");
	}
	size_t written = braced ? braced->children.size() : 0;
	string body;
	for (size_t i = 0; i < written; i++)
	{
		LowerConst value;
		if (!EvaluateLowerConst(*braced->children[i], value))
			throw OutsideBoundary("non-constant array element");
		bool is_zero_item = false;
		body += "  " + RenderConstItem(value, element, is_zero_item) +
			"\n";
	}
	if (array->bound > written)
		body += "  zero " +
			to_string((array->bound - written) * element_size) + "\n";
	return body;
}

// The definitions whose initial value comes from the sema-evaluated
// constant image: weak (instantiated static-member) definitions and
// storage definitions without their own initializer actions (9.4.2p3
// over an object-valued in-class initializer). Ordinary namespace
// objects keep the established zero-plus-dynamic-init shape.
bool LowerProgram::ImageBacked(const LowGlobalInfo& info) const
{
	if (!info.image || !info.defined)
		return false;
	if (info.weak)
		return true;
	return !info.node || info.node->children.empty();
}

const ClassInfo* LowerProgram::ProgramClass(
	const NamedTypeInfo* entity) const
{
	for (size_t u = 0; u < units_.size(); u++)
		if (const ClassInfo* found = units_[u]->classes.Find(entity))
			return found;
	return 0;
}

bool LowerProgram::AppendImageScalar(const ConstObject& image,
                                     const TypePtr& type,
                                     unsigned long long offset,
                                     unsigned long long& covered,
                                     string& out, bool in_class)
{
	TypePtr bare = RemoveTopCv(type);
	if (bare->kind == TK_POINTER)
	{
		map<unsigned long long, ConstPointer>::const_iterator slot =
			image.ptr_slots.find(offset);
		if (slot == image.ptr_slots.end())
			out += "  zero 8\n";  // null pointer slot
		else
		{
			const ConstPointer& value = slot->second;
			string symbol;
			if (value.sym_fn_type && in_class)
				// The checked references render function addresses in
				// array images, but a class image holding one
				// zero-fills and initializes dynamically.
				return false;
			if (value.sym_fn_type)
				symbol = FunctionRef(value.sym_scope, value.sym_name,
				                     value.sym_fn_type,
				                     value.sym_fn_spec);
			else if (value.sym_scope)
				symbol = GlobalRef(value.sym_scope, value.sym_name);
			else if (value.object && value.object->literal_node)
				symbol = StringLiteralRef(*value.object->literal_node);
			else
				return false;  // engine-internal object address
			out += "  ptr addr " + symbol;
			if (value.offset)
				out += " + " + to_string(value.offset);
			out += "\n";
		}
		covered = offset + 8;
		return true;
	}
	EFundamentalType fund = bare->kind == TK_ENUM
		? bare->named->enum_underlying : bare->fundamental;
	unsigned long long size = TypeSize(bare);
	if (offset + size > image.bytes.size())
		return false;
	if (IsFloatingFundamental(fund))
	{
		long double value = 0;
		if (fund == FT_FLOAT)
		{
			float f;
			std::memcpy(&f, &image.bytes[offset], sizeof(f));
			value = f;
		}
		else if (fund == FT_DOUBLE)
		{
			double d;
			std::memcpy(&d, &image.bytes[offset], sizeof(d));
			value = d;
		}
		else
			std::memcpy(&value, &image.bytes[offset], sizeof(value));
		out += "  " + LowerValueType(bare) + " " +
			RenderFloatConstant(value, fund) + "\n";
		covered = offset + size;
		return true;
	}
	unsigned long long bits = 0;
	for (unsigned long long i = 0; i < size && i < 8; i++)
		bits |= (unsigned long long)image.bytes[offset + i] << (8 * i);
	if (IsSignedIntegralFundamental(fund) && size < 8 &&
	    (bits & (1ull << (8 * size - 1))))
		bits |= ~((1ull << (8 * size)) - 1);
	out += "  " + LowerValueType(bare) + " " +
		RenderConstValue(ConstValue(fund, bits)) + "\n";
	covered = offset + size;
	return true;
}

bool LowerProgram::TryRenderImageItems(const ConstObject& image,
                                       const TypePtr& type,
                                       unsigned long long offset,
                                       unsigned long long& covered,
                                       string& out, bool in_class)
{
	TypePtr bare = RemoveTopCv(type);
	if (bare->kind == TK_ARRAY)
	{
		if (!bare->bound_known)
			return false;
		unsigned long long stride = TypeSize(RemoveTopCv(bare->target));
		for (unsigned long long i = 0; i < bare->bound; i++)
		{
			unsigned long long at = offset + i * stride;
			if (!TryRenderImageItems(image, bare->target, at, covered,
			                         out, in_class))
				return false;
			if (covered < at + stride)
			{
				out += "  zero " + to_string(at + stride - covered) +
					"\n";
				covered = at + stride;
			}
		}
		return true;
	}
	if (bare->kind == TK_CLASS)
	{
		// Base subobjects sit at offset zero (single inheritance);
		// fields flatten in ascending offset order (base chain first)
		// with zero runs for padding.
		vector<const NamedTypeInfo*> chain;
		for (const NamedTypeInfo* entity = bare->named; entity;
		     entity = entity->base_entity)
			chain.push_back(entity);
		for (size_t c = chain.size(); c > 0; c--)
		{
			const ClassInfo* cls = ProgramClass(chain[c - 1]);
			if (!cls)
				return false;
			if (cls->is_polymorphic)
				return false;  // vpointer images stay dynamic
			for (size_t f = 0; f < cls->fields.size(); f++)
			{
				const ClassField& field = cls->fields[f];
				if (field.name.empty())
					continue;
				if (field.is_bit_field)
					return false;
				unsigned long long at = offset + field.offset;
				if (covered < at)
				{
					out += "  zero " + to_string(at - covered) + "\n";
					covered = at;
				}
				if (!TryRenderImageItems(image, field.type, at,
				                         covered, out, true))
					return false;
			}
		}
		return true;
	}
	if (covered < offset)
	{
		out += "  zero " + to_string(offset - covered) + "\n";
		covered = offset;
	}
	return AppendImageScalar(image, bare, offset, covered, out, in_class);
}

// One attempt per global, made before BuildLifetimeHelpers decides
// whether the definition keeps its dynamic-init actions.
bool LowerProgram::EnsureImageText(LowGlobalInfo& info)
{
	if (info.image_state == 0)
	{
		string items;
		unsigned long long covered = 0;
		bool rendered = TryRenderImageItems(*info.image, info.type, 0,
		                                    covered, items);
		if (rendered)
		{
			unsigned long long size = TypeSize(RemoveTopCv(info.type));
			if (covered < size)
				items += "  zero " + to_string(size - covered) + "\n";
			if (items.empty())
				items = "  zero " + to_string(size) + "\n";
			info.image_text = items;
		}
		info.image_state = rendered ? 1 : 2;
	}
	return info.image_state == 1;
}

string LowerProgram::RenderGlobal(LowGlobalInfo& info)
{
	if (ImageBacked(info) && EnsureImageText(info))
		return "global @" + info.low_name + GlobalMetadata(info) +
			" = {\n" + info.image_text + "}";
	TypePtr inner = info.type;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	if (RemoveTopCv(inner)->kind == TK_CLASS)
		// Class objects zero-fill statically; construction runs in
		// @__cppgm_init.
		return "global @" + info.low_name + GlobalMetadata(info) +
			" = {\n  zero " + to_string(TypeSize(info.type)) + "\n}";
	if (RemoveTopCv(info.type)->kind == TK_ENUM && info.folded_const)
		// An enumeration constant object: every read folds to the
		// recorded value, so the storage keeps the structured zero
		// image (the checked references pin this shape).
		return "global @" + info.low_name + GlobalMetadata(info) +
			" = {\n  zero " + to_string(TypeSize(info.type)) + "\n}";
	if (IsReferenceType(info.type))
		return "global @" + info.low_name + " : ptr" +
			GlobalMetadata(info) + " = " + RenderScalarInit(info);
	if (info.type->kind == TK_ARRAY)
		return "global @" + info.low_name + GlobalMetadata(info) +
			" = {\n" + RenderArrayItems(info) + "}";
	return "global @" + info.low_name + " : " +
		LowerValueType(info.type) + GlobalMetadata(info) + " = " +
		RenderScalarInit(info);
}

