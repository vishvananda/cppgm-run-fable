// PA37 value pass: executable-edge constant/copy propagation, constant
// folding, algebraic identities, boolean compare cleanup, local
// reassociation, and terminator folding. At level 2 the same fixpoint
// tracks the contents of promotable (non-escaping, direct-access
// scalar) slots so loads fold without phi insertion.

#include <cmath>
#include <cstdlib>

#include "lowir/lowir_opt_internal.h"

namespace {

// Lattice value of a temp or tracked slot. KNOWN carries a replacement
// operand: a literal (with its spelling) or an alias to another temp.
// A literal with an empty spelling is numerically known (usable by
// folds) but never substituted into the program text.
struct Value
{
	enum EKind { TOP, KNOWN, VARYING };
	EKind kind = TOP;
	LowIROperand op;
	LowIRType slot_type;   // slot states: the storing instruction's type

	bool is_literal() const
	{
		return kind == KNOWN && op.kind == LOWIR_OPERAND_LITERAL;
	}
};

Value MakeVarying()
{
	Value value;
	value.kind = Value::VARYING;
	return value;
}

Value MakeKnown(const LowIROperand & op)
{
	Value value;
	value.kind = Value::KNOWN;
	value.op = op;
	return value;
}

typedef map<string, Value> SlotState;

struct FoldPass
{
	OptFunction & view;
	LowIRFunction & fn;
	int level;

	map<string, Value> temps;
	set<size_t> exec_blocks;
	set<std::pair<size_t, size_t> > exec_edges;

	// Promotable-slot tracking (level 2).
	bool allow_promotion = true;
	set<string> promotable;
	vector<SlotState> state_in;      // per block
	vector<SlotState> handler_in;    // merged at may-transfer points
	vector<vector<string> > stack_in;  // active handler labels
	vector<char> handler_stack_known;  // stack_in assigned at push site
	bool eh_stack_sane = true;

	explicit FoldPass(OptFunction & v)
		: view(v), fn(*v.fn), level(v.context->level)
	{
	}

	Value ResolveOperand(const LowIROperand & op) const;
	bool LiteralInt(const LowIROperand & op, unsigned long long & bits,
	                const LowIRType & type) const;
	Value EvaluateInstruction(const LowIRInstruction & ins,
	                          SlotState & slots);
	Value EvaluateUnary(const LowIRInstruction & ins,
	                    const LowIROperand & a) const;
	Value EvaluateBinary(const LowIRInstruction & ins,
	                     const LowIROperand & a,
	                     const LowIROperand & b) const;
	Value EvaluateCmp(const LowIRInstruction & ins,
	                  const LowIROperand & a,
	                  const LowIROperand & b) const;
	Value EvaluateConvert(const LowIRInstruction & ins,
	                      const LowIROperand & a) const;
	void FindPromotableSlots();
	void MergeSlotState(SlotState & into, const SlotState & from,
	                    bool & changed) const;
	void SeedHandlerState(const vector<string> & stack,
	                      const SlotState & slots, bool & changed);
	struct FixpointRound
	{
		bool changed = false;
		set<size_t> exec;   // processed this round (= executable)
		set<std::pair<size_t, size_t> > edges;
		vector<char> pending;
		vector<size_t> work;
	};
	void ProcessBlock(size_t b, FixpointRound & round);
	bool RunFixpoint();
	void SelectorTargets(const LowIRInstruction & ins,
	                     vector<size_t> & out) const;
	bool LexicallyBefore(const string & temp, size_t block,
	                     size_t index) const;
	bool SubstituteUses();
	bool MaterializePromotedPtrLoads();
	bool Reassociate();
	bool FoldTerminators();
};

bool ParseIntSpelling(const string & spelling, bool negated,
                      unsigned long long & bits)
{
	if(spelling.empty())
		return false;
	char * end = nullptr;
	unsigned long long value = strtoull(spelling.c_str(), &end, 0);
	if(end == spelling.c_str() || *end != '\0')
		return false;
	bits = negated ? (unsigned long long)0 - value : value;
	return true;
}

// Signed-flavored operations read the operand's width pattern as
// signed; on an unsigned-typed operand with the sign bit set that
// diverges from the type-canonical value this pass carries, so folds
// back off.
bool UnsignedSignBitSet(const LowIRType & type, unsigned long long bits)
{
	if(type.kind != LOWIR_TYPE_U8 && type.kind != LOWIR_TYPE_U16 &&
	   type.kind != LOWIR_TYPE_U32)
		return false;
	return (bits >> (type.integer_bits() - 1)) & 1;
}

}  // namespace

bool OperandIntValue(const LowIROperand & operand,
                     unsigned long long & bits)
{
	if(operand.kind != LOWIR_OPERAND_LITERAL ||
	   operand.literal_class != LOWIR_LITERAL_INT)
		return false;
	return ParseIntSpelling(operand.literal, operand.negated, bits);
}

bool OperandFloatValue(const LowIROperand & operand, long double & value)
{
	if(operand.kind != LOWIR_OPERAND_LITERAL)
		return false;
	if(operand.literal_class == LOWIR_LITERAL_INT)
	{
		unsigned long long bits = 0;
		if(!ParseIntSpelling(operand.literal, operand.negated, bits))
			return false;
		value = (long double)(long long)bits;
		return true;
	}
	if(operand.literal_class == LOWIR_LITERAL_NULLPTR)
		return false;
	if(operand.literal.empty())
		return false;
	char * end = nullptr;
	value = strtold(operand.literal.c_str(), &end);
	if(end == operand.literal.c_str())
		return false;
	if(operand.negated)
		value = -value;
	return true;
}

unsigned long long WrapIntToType(const LowIRType & type,
                                 unsigned long long bits)
{
	long width = type.integer_bits();
	if(width >= 64)
		return bits;
	unsigned long long mask = (1ull << width) - 1;
	bits &= mask;
	bool is_signed = type.kind != LOWIR_TYPE_U8 &&
		type.kind != LOWIR_TYPE_U16 && type.kind != LOWIR_TYPE_U32;
	if(is_signed && width > 1 && (bits >> (width - 1)))
		bits |= ~mask;
	return bits;
}

LowIROperand MakeIntLiteral(const LowIRType & type,
                            unsigned long long bits)
{
	bits = WrapIntToType(type, bits);
	LowIROperand op;
	op.kind = LOWIR_OPERAND_LITERAL;
	op.literal_class = LOWIR_LITERAL_INT;
	bool is_unsigned = type.kind == LOWIR_TYPE_U8 ||
		type.kind == LOWIR_TYPE_U16 || type.kind == LOWIR_TYPE_U32;
	if(is_unsigned || (long long)bits >= 0)
	{
		unsigned long long magnitude = bits;
		if(is_unsigned && type.integer_bits() < 64)
			magnitude &= (1ull << type.integer_bits()) - 1;
		op.literal = std::to_string(magnitude);
		return op;
	}
	op.negated = true;
	op.literal = std::to_string((unsigned long long)0 - bits);
	return op;
}

namespace {

Value FoldPass::ResolveOperand(const LowIROperand & op) const
{
	if(op.kind == LOWIR_OPERAND_LITERAL)
		return MakeKnown(op);
	if(op.kind != LOWIR_OPERAND_TEMP)
		return MakeVarying();
	map<string, Value>::const_iterator found = temps.find(op.name);
	if(found == temps.end() || found->second.kind == Value::TOP)
		return MakeKnown(op);
	if(found->second.kind == Value::VARYING)
	{
		Value value = MakeKnown(op);
		return value;
	}
	return found->second;
}

// Resolved operand as an integer literal, wrapped to `type`.
bool FoldPass::LiteralInt(const LowIROperand & op,
                          unsigned long long & bits,
                          const LowIRType & type) const
{
	if(!OperandIntValue(op, bits))
		return false;
	bits = WrapIntToType(type, bits);
	return true;
}

Value FoldPass::EvaluateUnary(const LowIRInstruction & ins,
                              const LowIROperand & a) const
{
	if(ins.operation == "decay")
		return MakeKnown(a);
	const LowIRType & type = ins.type;
	if(!type.is_integer() || type.kind == LOWIR_TYPE_I128)
	{
		if(ins.operation == "neg" && a.kind == LOWIR_OPERAND_LITERAL &&
		   a.literal_class != LOWIR_LITERAL_NULLPTR &&
		   !a.literal.empty() && type.is_float())
		{
			LowIROperand negated = a;
			negated.negated = !a.negated;
			return MakeKnown(negated);
		}
		return MakeVarying();
	}
	unsigned long long bits = 0;
	if(!LiteralInt(a, bits, type))
		return MakeVarying();
	if(ins.operation == "neg")
		return MakeKnown(MakeIntLiteral(type, (unsigned long long)0 - bits));
	if(ins.operation == "bitnot")
		return MakeKnown(MakeIntLiteral(type, ~bits));
	if(ins.operation == "not")
		return MakeKnown(MakeIntLiteral(type, bits == 0 ? 1 : 0));
	if(ins.operation == "bswap")
	{
		long width = type.integer_bits();
		if(width != 16 && width != 32 && width != 64)
			return MakeVarying();
		unsigned long long swapped = 0;
		for(long i = 0; i < width / 8; i++)
			swapped = (swapped << 8) | ((bits >> (8 * i)) & 0xff);
		return MakeKnown(MakeIntLiteral(type, swapped));
	}
	return MakeVarying();
}

Value FoldPass::EvaluateBinary(const LowIRInstruction & ins,
                               const LowIROperand & a,
                               const LowIROperand & b) const
{
	const LowIRType & type = ins.type;
	if(!type.is_integer() || type.kind == LOWIR_TYPE_I128)
		return MakeVarying();
	unsigned long long av = 0, bv = 0;
	bool a_lit = LiteralInt(a, av, type);
	bool b_lit = LiteralInt(b, bv, type);
	const string & op = ins.operation;

	if(a_lit && b_lit)
	{
		long width = type.integer_bits();
		unsigned long long ua = width >= 64 ? av
			: av & ((1ull << width) - 1);
		unsigned long long ub = width >= 64 ? bv
			: bv & ((1ull << width) - 1);
		if(op == "add")
			return MakeKnown(MakeIntLiteral(type, av + bv));
		if(op == "sub")
			return MakeKnown(MakeIntLiteral(type, av - bv));
		if(op == "mul")
			return MakeKnown(MakeIntLiteral(type, av * bv));
		if(op == "and")
			return MakeKnown(MakeIntLiteral(type, av & bv));
		if(op == "or")
			return MakeKnown(MakeIntLiteral(type, av | bv));
		if(op == "xor")
			return MakeKnown(MakeIntLiteral(type, av ^ bv));
		if(op == "div" || op == "mod")
		{
			if(UnsignedSignBitSet(type, av) ||
			   UnsignedSignBitSet(type, bv))
				return MakeVarying();
			long long sa = (long long)av, sb = (long long)bv;
			if(sb == 0 || (sb == -1 && sa == (-9223372036854775807ll - 1)))
				return MakeVarying();
			return MakeKnown(MakeIntLiteral(
				type, (unsigned long long)(op == "div" ? sa / sb
				                                       : sa % sb)));
		}
		if(op == "udiv" || op == "umod")
		{
			if(ub == 0)
				return MakeVarying();
			return MakeKnown(MakeIntLiteral(
				type, op == "udiv" ? ua / ub : ua % ub));
		}
		if(op == "shl" || op == "shr" || op == "ushr")
		{
			if(bv >= (unsigned long long)width)
				return MakeVarying();
			if(op == "shl")
				return MakeKnown(MakeIntLiteral(type, av << bv));
			if(op == "ushr")
				return MakeKnown(MakeIntLiteral(type, ua >> bv));
			if(UnsignedSignBitSet(type, av))
				return MakeVarying();
			return MakeKnown(MakeIntLiteral(
				type, (unsigned long long)((long long)av >> bv)));
		}
		return MakeVarying();
	}

	// Algebraic identities with one known constant side.
	unsigned long long all_ones = WrapIntToType(
		type, (unsigned long long)-1ll);
	if(b_lit)
	{
		if((op == "add" || op == "sub" || op == "or" || op == "xor" ||
		    op == "shl" || op == "shr" || op == "ushr") && bv == 0)
			return MakeKnown(a);
		if(op == "mul" && bv == WrapIntToType(type, 1))
			return MakeKnown(a);
		if(op == "and" && bv == all_ones)
			return MakeKnown(a);
		if((op == "mul" || op == "and") && bv == 0)
			return MakeKnown(MakeIntLiteral(type, 0));
	}
	if(a_lit)
	{
		if((op == "add" || op == "or" || op == "xor") && av == 0)
			return MakeKnown(b);
		if(op == "mul" && av == WrapIntToType(type, 1))
			return MakeKnown(b);
		if(op == "and" && av == all_ones)
			return MakeKnown(b);
		if((op == "mul" || op == "and") && av == 0)
			return MakeKnown(MakeIntLiteral(type, 0));
	}
	return MakeVarying();
}

Value FoldPass::EvaluateCmp(const LowIRInstruction & ins,
                            const LowIROperand & a,
                            const LowIROperand & b) const
{
	const string & op = ins.operation;
	LowIRType result_type;
	result_type.kind = LOWIR_TYPE_I64;

	// Identical non-float operands compare reflexively.
	if(!ins.type.is_float() && a.kind == LOWIR_OPERAND_TEMP &&
	   SameOperand(a, b))
	{
		bool truth = op == "eq" || op == "le" || op == "ge" ||
			op == "ule" || op == "uge";
		return MakeKnown(MakeIntLiteral(result_type, truth ? 1 : 0));
	}

	if(ins.type.is_float())
	{
		long double av = 0, bv = 0;
		if(!OperandFloatValue(a, av) || !OperandFloatValue(b, bv))
			return MakeVarying();
		// The backends round each literal to the operand type before
		// comparing; the fold must see the same values.
		if(ins.type.kind == LOWIR_TYPE_F32)
		{
			av = (float)av;
			bv = (float)bv;
		}
		else if(ins.type.kind == LOWIR_TYPE_F64)
		{
			av = (double)av;
			bv = (double)bv;
		}
		bool truth;
		if(op == "eq")
			truth = av == bv;
		else if(op == "ne")
			truth = !(av == bv);
		else if(op == "lt" || op == "ult")
			truth = av < bv;
		else if(op == "le" || op == "ule")
			truth = av <= bv;
		else if(op == "gt" || op == "ugt")
			truth = av > bv;
		else if(op == "ge" || op == "uge")
			truth = av >= bv;
		else
			return MakeVarying();
		return MakeKnown(MakeIntLiteral(result_type, truth ? 1 : 0));
	}

	if(!ins.type.is_integer() || ins.type.kind == LOWIR_TYPE_I128)
		return MakeVarying();
	unsigned long long av = 0, bv = 0;
	if(!LiteralInt(a, av, ins.type) || !LiteralInt(b, bv, ins.type))
		return MakeVarying();
	long width = ins.type.integer_bits();
	unsigned long long ua = width >= 64 ? av : av & ((1ull << width) - 1);
	unsigned long long ub = width >= 64 ? bv : bv & ((1ull << width) - 1);
	if((op == "lt" || op == "le" || op == "gt" || op == "ge") &&
	   (UnsignedSignBitSet(ins.type, av) ||
	    UnsignedSignBitSet(ins.type, bv)))
		return MakeVarying();
	long long sa = (long long)av, sb = (long long)bv;
	bool truth;
	if(op == "eq")
		truth = av == bv;
	else if(op == "ne")
		truth = av != bv;
	else if(op == "lt")
		truth = sa < sb;
	else if(op == "le")
		truth = sa <= sb;
	else if(op == "gt")
		truth = sa > sb;
	else if(op == "ge")
		truth = sa >= sb;
	else if(op == "ult")
		truth = ua < ub;
	else if(op == "ule")
		truth = ua <= ub;
	else if(op == "ugt")
		truth = ua > ub;
	else if(op == "uge")
		truth = ua >= ub;
	else
		return MakeVarying();
	return MakeKnown(MakeIntLiteral(result_type, truth ? 1 : 0));
}

Value FoldPass::EvaluateConvert(const LowIRInstruction & ins,
                                const LowIROperand & a) const
{
	const string & op = ins.operation;
	bool int_op = op == "sext" || op == "zext" || op == "trunc";
	if(int_op && ins.type.kind == ins.type2.kind &&
	   ins.type.obj_bytes == ins.type2.obj_bytes)
		return MakeKnown(a);
	if(int_op)
	{
		unsigned long long bits = 0;
		if(!OperandIntValue(a, bits) || !ins.type.is_integer() ||
		   ins.type.kind == LOWIR_TYPE_I128 ||
		   !ins.type2.is_integer() || ins.type2.kind == LOWIR_TYPE_I128)
			return MakeVarying();
		bits = WrapIntToType(ins.type2, bits);
		if(op == "sext" && UnsignedSignBitSet(ins.type2, bits))
			return MakeVarying();
		if(op == "zext")
		{
			long width = ins.type2.integer_bits();
			if(width < 64)
				bits &= (1ull << width) - 1;
		}
		return MakeKnown(MakeIntLiteral(ins.type, bits));
	}
	if(op == "sitofp" || op == "uitofp")
	{
		unsigned long long bits = 0;
		if(!OperandIntValue(a, bits) || !ins.type2.is_integer() ||
		   ins.type2.kind == LOWIR_TYPE_I128 || !ins.type.is_float())
			return MakeVarying();
		bits = WrapIntToType(ins.type2, bits);
		if(op == "sitofp" && UnsignedSignBitSet(ins.type2, bits))
			return MakeVarying();
		if(op == "uitofp")
		{
			// The unsigned read takes the width pattern zero-extended.
			long width = ins.type2.integer_bits();
			if(width < 64)
				bits &= (1ull << width) - 1;
		}
		long double value = op == "sitofp"
			? (long double)(long long)bits
			: (long double)bits;
		// Fold only exactly-representable integral values within the
		// signed 64-bit spelling range; spell them as `<int>.0` with
		// the destination type's suffix.
		if(value < -9223372036854775808.0L ||
		   value >= 9223372036854775808.0L)
			return MakeVarying();
		if(value != (long double)(long long)value)
			return MakeVarying();
		long long ivalue = (long long)value;
		LowIROperand folded;
		folded.kind = LOWIR_OPERAND_LITERAL;
		folded.negated = ivalue < 0;
		folded.literal = std::to_string(ivalue < 0 ? -ivalue : ivalue) +
			".0";
		folded.literal_class = LOWIR_LITERAL_F64;
		if(ins.type.kind == LOWIR_TYPE_F32)
		{
			folded.literal += "f";
			folded.literal_class = LOWIR_LITERAL_F32;
		}
		else if(ins.type.is_f80())
		{
			folded.literal += "L";
			folded.literal_class = LOWIR_LITERAL_F80;
		}
		return MakeKnown(folded);
	}
	return MakeVarying();
}

// Marks slots eligible for level-2 promotion: scalar/ptr type and only
// direct load/store uses anywhere in the body.
void FoldPass::FindPromotableSlots()
{
	promotable.clear();
	if(level < 2 || !allow_promotion)
		return;
	map<string, bool> eligible;
	for(size_t i = 0; i < fn.slots.size(); i++)
	{
		const LowIRType & type = fn.slots[i].type;
		eligible[fn.slots[i].name] =
			!type.is_obj() && !type.is_void() && !type.is_f80();
	}
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			for(size_t o = 0; o < ins.operands.size(); o++)
			{
				const LowIROperand & op = ins.operands[o];
				if(op.kind != LOWIR_OPERAND_SLOT)
					continue;
				bool direct =
					(ins.opcode == LOWIR_INS_LOAD && o == 0) ||
					(ins.opcode == LOWIR_INS_STORE && o == 1);
				if(!direct)
					eligible[op.name] = false;
			}
			for(size_t o = 0; o < ins.switch_values.size(); o++)
				if(ins.switch_values[o].kind == LOWIR_OPERAND_SLOT)
					eligible[ins.switch_values[o].name] = false;
		}
	for(map<string, bool>::const_iterator it = eligible.begin();
	    it != eligible.end(); ++it)
		if(it->second)
			promotable.insert(it->first);
}

void FoldPass::MergeSlotState(SlotState & into, const SlotState & from,
                              bool & changed) const
{
	for(set<string>::const_iterator s = promotable.begin();
	    s != promotable.end(); ++s)
	{
		SlotState::const_iterator theirs = from.find(*s);
		Value incoming = theirs == from.end() ? Value() : theirs->second;
		SlotState::iterator mine = into.find(*s);
		if(mine == into.end())
		{
			if(incoming.kind != Value::TOP)
			{
				into[*s] = incoming;
				changed = true;
			}
			continue;
		}
		Value & current = mine->second;
		if(incoming.kind == Value::TOP ||
		   current.kind == Value::VARYING)
			continue;
		if(current.kind == Value::TOP)
		{
			current = incoming;
			changed = true;
			continue;
		}
		if(incoming.kind == Value::VARYING ||
		   !SameOperand(current.op, incoming.op) ||
		   current.slot_type.kind != incoming.slot_type.kind)
		{
			current = MakeVarying();
			changed = true;
		}
	}
}

void FoldPass::SeedHandlerState(const vector<string> & stack,
                                const SlotState & slots, bool & changed)
{
	for(size_t h = 0; h < stack.size(); h++)
	{
		map<string, size_t>::const_iterator target =
			view.block_index.find(stack[h]);
		if(target == view.block_index.end())
			continue;
		MergeSlotState(handler_in[target->second], slots, changed);
	}
}

Value FoldPass::EvaluateInstruction(const LowIRInstruction & ins,
                                    SlotState & slots)
{
	switch(ins.opcode)
	{
	case LOWIR_INS_CONST:
	{
		const LowIROperand & lit = ins.operands[0];
		// Pointer-typed constants stay pinned to their defining temp
		// (storage positions cannot spell literals).
		if(ins.type.kind == LOWIR_TYPE_PTR)
			return MakeVarying();
		return MakeKnown(lit);
	}
	case LOWIR_INS_COPY:
		return ResolveOperand(ins.operands[0]);
	case LOWIR_INS_UNARY:
		return EvaluateUnary(ins, ResolveOperand(ins.operands[0]).op);
	case LOWIR_INS_BINARY:
		return EvaluateBinary(ins, ResolveOperand(ins.operands[0]).op,
		                      ResolveOperand(ins.operands[1]).op);
	case LOWIR_INS_CMP:
	{
		Value a = ResolveOperand(ins.operands[0]);
		Value b = ResolveOperand(ins.operands[1]);
		Value folded = EvaluateCmp(ins, a.op, b.op);
		if(folded.kind != Value::VARYING)
			return folded;
		// Boolean compare cleanup: cmp ne/eq of an i64 boolean
		// against 0/1 is the boolean itself.
		if(ins.type.kind == LOWIR_TYPE_I64)
		{
			for(int side = 0; side < 2; side++)
			{
				const LowIROperand & value = side ? b.op : a.op;
				const LowIROperand & other = side ? a.op : b.op;
				unsigned long long bits = 0;
				if(value.kind != LOWIR_OPERAND_TEMP ||
				   !OperandIntValue(other, bits))
					continue;
				map<string, OptInsRef>::const_iterator def =
					view.temp_defs.find(value.name);
				if(def == view.temp_defs.end())
					continue;
				const LowIRInstruction & producer =
					fn.blocks[def->second.block]
						.instructions[def->second.index];
				if(producer.opcode != LOWIR_INS_CMP)
					continue;
				if((ins.operation == "ne" && bits == 0) ||
				   (ins.operation == "eq" && bits == 1))
					return MakeKnown(value);
			}
		}
		return MakeVarying();
	}
	case LOWIR_INS_CONVERT:
		return EvaluateConvert(ins,
		                       ResolveOperand(ins.operands[0]).op);
	case LOWIR_INS_LOAD:
	{
		const LowIROperand & storage = ins.operands[0];
		if(storage.kind == LOWIR_OPERAND_SLOT &&
		   promotable.count(storage.name))
		{
			SlotState::const_iterator found = slots.find(storage.name);
			if(found != slots.end() &&
			   found->second.kind == Value::KNOWN &&
			   found->second.slot_type.kind == ins.type.kind)
				return found->second;
			return MakeVarying();
		}
		return MakeVarying();
	}
	case LOWIR_INS_STORE:
	{
		const LowIROperand & storage = ins.operands[1];
		if(storage.kind == LOWIR_OPERAND_SLOT &&
		   promotable.count(storage.name))
		{
			Value value = ResolveOperand(ins.operands[0]);
			if(value.kind != Value::KNOWN)
				value = MakeVarying();
			value.slot_type = ins.type;
			slots[storage.name] = value;
		}
		return MakeVarying();
	}
	default:
		return MakeVarying();
	}
}

void FoldPass::SelectorTargets(const LowIRInstruction & ins,
                               vector<size_t> & out) const
{
	out.clear();
	if(ins.opcode == LOWIR_INS_JUMP)
	{
		out.push_back(view.block_index.find(ins.block_targets[0])->second);
		return;
	}
	if(ins.opcode == LOWIR_INS_BRANCH)
	{
		Value selector = ResolveOperand(ins.operands[0]);
		unsigned long long bits = 0;
		if(selector.is_literal() && OperandIntValue(selector.op, bits))
		{
			out.push_back(view.block_index
				.find(ins.block_targets[bits ? 0 : 1])->second);
			return;
		}
		for(size_t t = 0; t < ins.block_targets.size(); t++)
			out.push_back(
				view.block_index.find(ins.block_targets[t])->second);
		return;
	}
	if(ins.opcode == LOWIR_INS_SWITCH)
	{
		Value selector = ResolveOperand(ins.operands[0]);
		unsigned long long bits = 0;
		if(selector.is_literal() && OperandIntValue(selector.op, bits))
		{
			size_t chosen = 0;  // default target
			bool decidable = true;
			for(size_t c = 0; c < ins.switch_values.size(); c++)
			{
				unsigned long long cv = 0;
				if(!OperandIntValue(ins.switch_values[c], cv))
				{
					decidable = false;
					break;
				}
				if(cv == bits)
				{
					chosen = c + 1;
					break;
				}
			}
			if(decidable)
			{
				out.push_back(view.block_index
					.find(ins.block_targets[chosen])->second);
				return;
			}
		}
		for(size_t t = 0; t < ins.block_targets.size(); t++)
			out.push_back(
				view.block_index.find(ins.block_targets[t])->second);
	}
}

bool SameValue(const Value & a, const Value & b)
{
	if(a.kind != b.kind)
		return false;
	return a.kind != Value::KNOWN || SameOperand(a.op, b.op);
}

bool FoldPass::RunFixpoint()
{
	FindPromotableSlots();
	state_in.assign(fn.blocks.size(), SlotState());
	handler_in.assign(fn.blocks.size(), SlotState());
	stack_in.assign(fn.blocks.size(), vector<string>());
	handler_stack_known.assign(fn.blocks.size(), 0);

	for(int iter = 0; iter < 64; iter++)
	{
		map<string, Value> prior_temps;
		prior_temps.swap(temps);
		FixpointRound round;
		round.pending.assign(fn.blocks.size(), 0);
		round.work.push_back(0);
		round.pending[0] = 1;

		// Entry slot state: unknown storage reads stay loads.
		SlotState entry_state;
		for(set<string>::const_iterator s = promotable.begin();
		    s != promotable.end(); ++s)
			entry_state[*s] = MakeVarying();
		bool merge_ignored = false;
		MergeSlotState(state_in[0], entry_state, merge_ignored);

		while(!round.work.empty())
		{
			size_t b = round.work.back();
			round.work.pop_back();
			round.pending[b] = 0;
			ProcessBlock(b, round);
		}

		bool temps_stable = temps.size() == prior_temps.size();
		if(temps_stable)
			for(map<string, Value>::const_iterator it = temps.begin();
			    temps_stable && it != temps.end(); ++it)
			{
				map<string, Value>::const_iterator prior =
					prior_temps.find(it->first);
				temps_stable = prior != prior_temps.end() &&
					SameValue(prior->second, it->second);
			}
		bool stable = round.exec == exec_blocks &&
			round.edges == exec_edges && temps_stable && !round.changed;
		exec_blocks.swap(round.exec);
		exec_edges.swap(round.edges);
		if(stable)
			return true;
	}
	return false;
}

void FoldPass::ProcessBlock(size_t b, FixpointRound & round)
{
	bool & changed = round.changed;
	set<size_t> & new_exec = round.exec;
	set<std::pair<size_t, size_t> > & new_edges = round.edges;
	vector<char> & pending = round.pending;
	vector<size_t> & work = round.work;
	new_exec.insert(b);
	SlotState slots = state_in[b];
	vector<string> stack = stack_in[b];
	const LowIRBlock & block = fn.blocks[b];
	for(size_t k = 0; k < block.instructions.size(); k++)
	{
		const LowIRInstruction & ins = block.instructions[k];
		if(ins.opcode == LOWIR_INS_EH_TRY ||
		   ins.opcode == LOWIR_INS_EH_CLEANUP)
		{
			// The transfer pops the pushed region, so the handler runs
			// under the stack active at the push site; its own throw
			// and resume points seed the enclosing handlers.
			map<string, size_t>::const_iterator target =
				view.block_index.find(ins.block_targets[0]);
			if(target != view.block_index.end())
			{
				size_t h = target->second;
				if(!handler_stack_known[h] && stack_in[h].empty())
				{
					handler_stack_known[h] = 1;
					stack_in[h] = stack;
				}
				else if(stack_in[h] != stack)
					eh_stack_sane = false;
			}
			stack.push_back(ins.block_targets[0]);
		}
		else if(ins.opcode == LOWIR_INS_EH_END)
		{
			if(!stack.empty())
				stack.pop_back();
			else
				eh_stack_sane = false;
		}
		if(MayTransferToHandler(*view.context, ins))
			SeedHandlerState(stack, slots, changed);
		Value value = EvaluateInstruction(ins, slots);
		if(!ins.result.empty())
		{
			map<string, Value>::iterator known = temps.find(ins.result);
			if(known == temps.end())
				temps[ins.result] = value;
			else if(known->second.kind == Value::KNOWN &&
			        !SameValue(known->second, value))
				known->second = MakeVarying();
		}
	}
	if(block.instructions.empty())
		return;
	vector<size_t> succs;
	SelectorTargets(block.instructions.back(), succs);
	for(size_t s = 0; s < succs.size(); s++)
	{
		new_edges.insert(std::make_pair(b, succs[s]));
		bool merged = false;
		MergeSlotState(state_in[succs[s]], slots, merged);
		if(stack_in[succs[s]].empty() && !stack.empty() &&
		   new_exec.count(succs[s]) == 0)
			stack_in[succs[s]] = stack;
		if((merged || !new_exec.count(succs[s])) && !pending[succs[s]])
		{
			pending[succs[s]] = 1;
			work.push_back(succs[s]);
		}
	}
	// Handler targets of executable eh pushes stay reachable.
	for(size_t k = 0; k < block.instructions.size(); k++)
	{
		const LowIRInstruction & ins = block.instructions[k];
		if(ins.opcode != LOWIR_INS_EH_TRY &&
		   ins.opcode != LOWIR_INS_EH_CLEANUP)
			continue;
		map<string, size_t>::const_iterator target =
			view.block_index.find(ins.block_targets[0]);
		if(target == view.block_index.end())
			continue;
		size_t h = target->second;
		new_edges.insert(std::make_pair(b, h));
		bool merged = false;
		MergeSlotState(state_in[h], handler_in[h], merged);
		if((merged || !new_exec.count(h)) && !pending[h])
		{
			pending[h] = 1;
			work.push_back(h);
		}
	}
}

bool FoldPass::LexicallyBefore(const string & temp, size_t block,
                               size_t index) const
{
	map<string, OptInsRef>::const_iterator def = view.temp_defs.find(temp);
	if(def == view.temp_defs.end())
		return view.param_names.count(temp) != 0;
	if(def->second.block != block)
		return def->second.block < block;
	return def->second.index < index;
}

// Substitutes resolved operand values into every legal position.
bool FoldPass::SubstituteUses()
{
	bool changed = false;
	for(set<size_t>::const_iterator b = exec_blocks.begin();
	    b != exec_blocks.end(); ++b)
	{
		LowIRBlock & block = fn.blocks[*b];
		for(size_t k = 0; k < block.instructions.size(); k++)
		{
			LowIRInstruction & ins = block.instructions[k];
			for(int list = 0; list < 2; list++)
			{
				vector<LowIROperand> & operands =
					list ? ins.switch_values : ins.operands;
				for(size_t o = 0; o < operands.size(); o++)
				{
					LowIROperand & at = operands[o];
					if(at.kind != LOWIR_OPERAND_TEMP)
						continue;
					bool storage_position = !list &&
						((ins.opcode == LOWIR_INS_LOAD && o == 0) ||
						 (ins.opcode == LOWIR_INS_STORE && o == 1));
					Value value = ResolveOperand(at);
					if(value.kind != Value::KNOWN ||
					   SameOperand(value.op, at))
						continue;
					if(value.op.kind == LOWIR_OPERAND_LITERAL &&
					   (storage_position || value.op.literal.empty()))
						continue;
					if(value.op.kind == LOWIR_OPERAND_TEMP &&
					   !LexicallyBefore(value.op.name, *b, k))
						continue;
					at = value.op;
					changed = true;
				}
			}
			if(ins.opcode == LOWIR_INS_CALL && ins.callee_is_temp)
			{
				LowIROperand callee;
				callee.kind = LOWIR_OPERAND_TEMP;
				callee.name = ins.callee;
				Value value = ResolveOperand(callee);
				if(value.kind == Value::KNOWN &&
				   value.op.kind == LOWIR_OPERAND_TEMP &&
				   value.op.name != ins.callee &&
				   LexicallyBefore(value.op.name, *b, k))
				{
					ins.callee = value.op.name;
					changed = true;
				}
			}
		}
	}
	return changed;
}

// A promotable pointer slot holding a literal cannot substitute into
// storage positions, so its loads rematerialize as `const ptr`
// instructions in place (keeping their result names); the slot's
// stores then die.
bool FoldPass::MaterializePromotedPtrLoads()
{
	if(promotable.empty())
		return false;
	bool changed = false;
	for(set<size_t>::const_iterator b = exec_blocks.begin();
	    b != exec_blocks.end(); ++b)
	{
		SlotState slots = state_in[*b];
		LowIRBlock & block = fn.blocks[*b];
		for(size_t k = 0; k < block.instructions.size(); k++)
		{
			LowIRInstruction & ins = block.instructions[k];
			bool qualifies = ins.opcode == LOWIR_INS_LOAD &&
				ins.type.kind == LOWIR_TYPE_PTR &&
				ins.operands[0].kind == LOWIR_OPERAND_SLOT &&
				promotable.count(ins.operands[0].name);
			Value state;
			if(qualifies)
			{
				SlotState::const_iterator found =
					slots.find(ins.operands[0].name);
				if(found != slots.end())
					state = found->second;
			}
			Value ignored = EvaluateInstruction(ins, slots);
			(void)ignored;
			if(!qualifies || state.kind != Value::KNOWN ||
			   state.op.kind != LOWIR_OPERAND_LITERAL ||
			   state.slot_type.kind != LOWIR_TYPE_PTR ||
			   state.op.literal.empty())
				continue;
			LowIRInstruction rewritten;
			rewritten.opcode = LOWIR_INS_CONST;
			rewritten.result = ins.result;
			rewritten.type = ins.type;
			rewritten.operands.push_back(state.op);
			ins = rewritten;
			changed = true;
		}
	}
	return changed;
}

// Local reassociation: same-op integer chains with constant tails
// combine into the later instruction, which keeps its name.
bool FoldPass::Reassociate()
{
	bool changed = false;
	for(set<size_t>::const_iterator b = exec_blocks.begin();
	    b != exec_blocks.end(); ++b)
	{
		LowIRBlock & block = fn.blocks[*b];
		for(size_t k = 0; k < block.instructions.size(); k++)
		{
			LowIRInstruction & ins = block.instructions[k];
			if(ins.opcode != LOWIR_INS_BINARY ||
			   !ins.type.is_integer() ||
			   ins.type.kind == LOWIR_TYPE_I128)
				continue;
			const string & op = ins.operation;
			if(op != "add" && op != "mul" && op != "and" &&
			   op != "or" && op != "xor")
				continue;
			// Normalize: variable left, constant right.
			unsigned long long cv = 0;
			int const_side = -1;
			if(OperandIntValue(ins.operands[1], cv))
				const_side = 1;
			else if(OperandIntValue(ins.operands[0], cv))
				const_side = 0;
			if(const_side < 0)
				continue;
			LowIROperand var = ins.operands[const_side ? 0 : 1];
			if(var.kind != LOWIR_OPERAND_TEMP)
				continue;
			map<string, OptInsRef>::const_iterator def =
				view.temp_defs.find(var.name);
			if(def == view.temp_defs.end() ||
			   !exec_blocks.count(def->second.block))
				continue;
			const LowIRInstruction & producer =
				fn.blocks[def->second.block]
					.instructions[def->second.index];
			if(producer.opcode != LOWIR_INS_BINARY ||
			   producer.operation != op ||
			   producer.type.kind != ins.type.kind)
				continue;
			unsigned long long pv = 0;
			int producer_const = -1;
			if(OperandIntValue(producer.operands[1], pv))
				producer_const = 1;
			else if(OperandIntValue(producer.operands[0], pv))
				producer_const = 0;
			if(producer_const < 0)
				continue;
			const LowIROperand & inner =
				producer.operands[producer_const ? 0 : 1];
			if(inner.kind == LOWIR_OPERAND_TEMP &&
			   !LexicallyBefore(inner.name, *b, k))
				continue;
			unsigned long long combined;
			cv = WrapIntToType(ins.type, cv);
			pv = WrapIntToType(ins.type, pv);
			if(op == "add")
				combined = pv + cv;
			else if(op == "mul")
				combined = pv * cv;
			else if(op == "and")
				combined = pv & cv;
			else if(op == "or")
				combined = pv | cv;
			else
				combined = pv ^ cv;
			ins.operands[0] = inner;
			ins.operands[1] = MakeIntLiteral(ins.type, combined);
			changed = true;
		}
	}
	return changed;
}

bool FoldPass::FoldTerminators()
{
	bool changed = false;
	for(set<size_t>::const_iterator b = exec_blocks.begin();
	    b != exec_blocks.end(); ++b)
	{
		LowIRBlock & block = fn.blocks[*b];
		if(block.instructions.empty())
			continue;
		LowIRInstruction & term = block.instructions.back();
		if(term.opcode != LOWIR_INS_BRANCH &&
		   term.opcode != LOWIR_INS_SWITCH)
			continue;
		vector<size_t> succs;
		SelectorTargets(term, succs);
		bool same = !succs.empty();
		for(size_t s = 1; s < succs.size(); s++)
			if(succs[s] != succs[0])
				same = false;
		if(!same)
			continue;
		LowIRInstruction jump;
		jump.opcode = LOWIR_INS_JUMP;
		jump.block_targets.push_back(fn.blocks[succs[0]].label);
		term = jump;
		changed = true;
	}
	return changed;
}

}  // namespace

bool FoldAndPropagate(OptFunction & view)
{
	if(!view.optimizable)
		return false;
	FoldPass pass(view);
	if(!pass.RunFixpoint())
		return false;
	if(!pass.eh_stack_sane && !pass.promotable.empty())
	{
		// The handler seeding could not follow the region stack, so
		// slot promotion is not safe here; rerun with tracking off.
		FoldPass fallback(view);
		fallback.allow_promotion = false;
		if(!fallback.RunFixpoint())
			return false;
		bool changed = false;
		changed |= fallback.SubstituteUses();
		changed |= fallback.Reassociate();
		changed |= fallback.FoldTerminators();
		return changed;
	}
	bool changed = false;
	changed |= pass.SubstituteUses();
	changed |= pass.MaterializePromotedPtrLoads();
	changed |= pass.Reassociate();
	changed |= pass.FoldTerminators();
	return changed;
}
