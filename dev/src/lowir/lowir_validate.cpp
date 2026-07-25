#include "lowir/lowir_validate.h"

#include <set>
#include <stdexcept>

using std::runtime_error;
using std::set;

namespace {

void fail(const string & what)
{
	throw runtime_error("LowIR validation error: " + what);
}

bool value_in(const string & value, const char * const * options)
{
	for(size_t i = 0; options[i]; ++i)
		if(value == options[i])
			return true;
	return false;
}

const char * const kFunctionRoles[] = {
	"entry", "init", "fini", "eh_unhandled", "eh_allocate_exception",
	"eh_begin_catch", "eh_call_unexpected", "eh_current_exception_type",
	"eh_end_catch", "eh_rethrow", "eh_throw", "eh_personality",
	"eh_resume", "va_start", "alloca", nullptr};
const char * const kGlobalRoles[] = {"eh_top", "eh_value", "eh_type",
	nullptr};
const char * const kLinkages[] = {"c", "cpp", nullptr};
const char * const kBindings[] = {"internal", "strong", "weak", nullptr};
const char * const kYesNo[] = {"yes", "no", nullptr};
const char * const kStorages[] = {"readonly", "thread_local", nullptr};
const char * const kArities[] = {"fixed", "variadic", "prototype_relaxed",
	nullptr};
const char * const kEffects[] = {"readnone", "readonly", "readwrite",
	nullptr};
const char * const kUnwinds[] = {"may", "no", nullptr};
const char * const kReturns[] = {"returns", "noreturn", nullptr};
const char * const kPasses[] = {"direct", "indirect_result", "by_address",
	"reference", "decay", "gpr_pair", nullptr};
const char * const kCaptures[] = {"nocapture", "maycapture", nullptr};
const char * const kAccesses[] = {"none", "read", "write", "readwrite",
	nullptr};
const char * const kAliases[] = {"noalias", nullptr};
const char * const kProjections[] = {"array_element", "field",
	"base_subobject", "reference_field", nullptr};

// Validates one call-boundary metadata item shared by function headers
// and call signatures; returns false if the key is not a boundary key.
bool check_boundary_metadata_item(const string & key, const string & value)
{
	if(key == "arity")
	{
		if(!value_in(value, kArities))
			fail("unknown arity value: " + value);
		return true;
	}
	if(key == "effects")
	{
		if(!value_in(value, kEffects))
			fail("unknown effects value: " + value);
		return true;
	}
	if(key == "unwind")
	{
		if(!value_in(value, kUnwinds))
			fail("unknown unwind value: " + value);
		return true;
	}
	if(key == "return")
	{
		if(!value_in(value, kReturns))
			fail("unknown return value: " + value);
		return true;
	}
	return false;
}

// Validates one top-level symbol metadata item shared by globals and
// functions (role checked separately); returns false when unrecognized.
bool check_symbol_metadata_item(const string & key, const string & value,
                                const string & low_name)
{
	if(key == "linkage")
	{
		if(!value_in(value, kLinkages))
			fail("unknown linkage value: " + value);
		return true;
	}
	if(key == "binding")
	{
		if(!value_in(value, kBindings))
			fail("unknown binding value: " + value);
		return true;
	}
	if(key == "keep_alias" || key == "prefer_local" ||
	   key == "object_root" || key == "trivial_lifecycle")
	{
		if(!value_in(value, kYesNo))
			fail("bad " + key + " value: " + value);
		return true;
	}
	if(key == "object")
	{
		// PA29: `object=@low` marks a weak entity that merges across
		// translation units by its own (deterministic) low name; any
		// other @-spelling would merge under a name nobody checks.
		if(value.empty() || value == "@")
			fail("bad object symbol value: " + value);
		if(value[0] == '@' && value.substr(1) != low_name)
			fail("object self-merge key names a different symbol: " +
			     value);
		return true;
	}
	return false;
}

void check_parameter_metadata(const LowIRParam & param, size_t position,
                              const LowIRType & return_type)
{
	for(size_t i = 0; i < param.metadata.items.size(); ++i)
	{
		const string & key = param.metadata.items[i].first;
		const string & value = param.metadata.items[i].second;
		bool needs_ptr = false;
		if(key == "pass")
		{
			if(!value_in(value, kPasses))
				fail("unknown pass value: " + value);
			// gpr_pair rides a direct 9..16-byte object parameter.
			needs_ptr = value != "direct" && value != "gpr_pair";
			if(value == "indirect_result")
			{
				if(position != 0)
					fail("indirect_result parameter must be first");
				if(!return_type.is_void())
					fail("indirect_result requires a void return type");
			}
			if(value == "gpr_pair" &&
			   (param.type.kind != LOWIR_TYPE_OBJ ||
			    param.type.obj_bytes <= 8 || param.type.obj_bytes > 16))
				fail("gpr_pair requires a 9..16-byte object parameter");
		}
		else if(key == "capture")
		{
			if(!value_in(value, kCaptures))
				fail("unknown capture value: " + value);
			needs_ptr = true;
		}
		else if(key == "access")
		{
			if(!value_in(value, kAccesses))
				fail("unknown access value: " + value);
			needs_ptr = true;
		}
		else if(key == "alias")
		{
			if(!value_in(value, kAliases))
				fail("unknown alias value: " + value);
			needs_ptr = true;
		}
		else
			fail("unknown parameter metadata key: " + key);
		if(needs_ptr && param.type.kind != LOWIR_TYPE_PTR)
			fail("parameter metadata " + key + "=" + value +
			     " requires a ptr parameter");
	}
}

void check_signature_boundary(const LowIRCallSignature & signature)
{
	for(size_t i = 0; i < signature.metadata.items.size(); ++i)
	{
		const string & key = signature.metadata.items[i].first;
		const string & value = signature.metadata.items[i].second;
		if(!check_boundary_metadata_item(key, value))
			fail("metadata key not valid on call signatures: " + key);
	}
	for(size_t i = 0; i < signature.params.size(); ++i)
		check_parameter_metadata(signature.params[i], i,
		                         signature.return_type);
}

struct Validator
{
	const LowIRProgram & program;
	bool require_entry;
	LowIRProgramInfo info;
	map<string, int> role_owners;
	map<string, string> tls_wrappers;  // target global -> wrapper

	explicit Validator(const LowIRProgram & p, bool require_entry_hook)
		: program(p), require_entry(require_entry_hook) {}

	void check_symbols()
	{
		set<string> names;
		for(size_t i = 0; i < program.globals.size(); ++i)
		{
			const LowIRGlobal & global = program.globals[i];
			if(!names.insert(global.name).second)
				fail("duplicate top-level symbol: @" + global.name);
			info.globals[global.name] = &global;
		}
		for(size_t i = 0; i < program.functions.size(); ++i)
		{
			const LowIRFunction & function = program.functions[i];
			if(!names.insert(function.name).second)
				fail("duplicate top-level symbol: @" + function.name);
			info.functions[function.name] = &function;
		}
	}

	bool symbol_exists(const string & name) const
	{
		return info.globals.count(name) || info.functions.count(name);
	}

	void check_aliases()
	{
		set<string> spellings;
		for(size_t i = 0; i < program.aliases.size(); ++i)
		{
			const LowIRAlias & alias = program.aliases[i];
			if(!spellings.insert(alias.object_symbol).second)
			{
				string targets;
				for(size_t j = 0; j < program.aliases.size(); ++j)
					if(program.aliases[j].object_symbol ==
					   alias.object_symbol)
						targets += " @" + program.aliases[j].target;
				fail("duplicate object alias: " + alias.object_symbol +
				     " targets:" + targets);
			}
			if(!symbol_exists(alias.target))
				fail("object alias target is not a top-level symbol: @" +
				     alias.target);
		}
	}

	void claim_role(const string & role)
	{
		if(++role_owners[role] > 1)
			fail("role owned by more than one symbol: " + role);
	}

	void check_global_metadata(const LowIRGlobal & global)
	{
		for(size_t i = 0; i < global.metadata.items.size(); ++i)
		{
			const string & key = global.metadata.items[i].first;
			const string & value = global.metadata.items[i].second;
			if(key == "role")
			{
				if(!value_in(value, kGlobalRoles))
					fail("bad global role: " + value);
				claim_role(value);
			}
			else if(key == "storage")
			{
				if(!value_in(value, kStorages))
					fail("unknown storage value: " + value);
			}
			else if(!check_symbol_metadata_item(key, value, global.name))
				fail("unknown global metadata key: " + key);
		}
	}

	void check_tls_for(const LowIRFunction & function, const string & value)
	{
		if(value.size() < 2 || value[0] != '@')
			fail("tls_for requires a @global target");
		string target = value.substr(1);
		map<string, const LowIRGlobal *>::const_iterator found =
			info.globals.find(target);
		if(found == info.globals.end())
			fail("tls_for target is not a global: " + value);
		if(found->second->metadata.find("storage") != "thread_local")
			fail("tls_for target is not thread-local: " + value);
		if(!tls_wrappers.insert(
			std::make_pair(target, function.name)).second)
			fail("multiple tls_for wrappers for: " + value);
	}

	void check_function_metadata(const LowIRFunction & function)
	{
		for(size_t i = 0; i < function.metadata.items.size(); ++i)
		{
			const string & key = function.metadata.items[i].first;
			const string & value = function.metadata.items[i].second;
			if(key == "role")
			{
				if(!value_in(value, kFunctionRoles))
					fail("bad function role: " + value);
				claim_role(value);
			}
			else if(key == "tls_for")
				check_tls_for(function, value);
			else if(check_boundary_metadata_item(key, value))
				continue;
			else if(!check_symbol_metadata_item(key, value,
			                                    function.name))
				fail("unknown function metadata key: " + key);
		}
		for(size_t i = 0; i < function.params.size(); ++i)
			check_parameter_metadata(function.params[i], i,
			                         function.return_type);
	}

	void check_data_item(const LowIRDataItem & item)
	{
		if(item.kind == LOWIR_DATA_ADDR)
		{
			if(!symbol_exists(item.symbol))
				fail("data address target is not a top-level symbol: @" +
				     item.symbol);
			return;
		}
		if(item.kind == LOWIR_DATA_SCALAR &&
		   (item.type.is_void() || item.type.is_obj()))
			fail("structured data items require scalar types");
	}

	void check_global(const LowIRGlobal & global)
	{
		check_global_metadata(global);
		if(global.has_type &&
		   (global.type.is_void() || global.type.is_obj()))
			fail("global storage requires a scalar or pointer type: @" +
			     global.name);
		if(global.init == LOWIR_GLOBAL_ADDR &&
		   !symbol_exists(global.addr_symbol))
			fail("global address initializer target missing: @" +
			     global.addr_symbol);
		for(size_t i = 0; i < global.items.size(); ++i)
			check_data_item(global.items[i]);
	}

	void check_convert(const LowIRInstruction & ins)
	{
		const LowIRType & dst = ins.type;
		const LowIRType & src = ins.type2;
		const string & op = ins.operation;
		if(op == "zext" || op == "sext" || op == "trunc")
		{
			if(!dst.is_integer() || !src.is_integer())
				fail("integer conversion requires integer types");
			// Identity widths are accepted as no-op conversions (the
			// PA37 optimizer's cleanup input can spell them).
			if(op == "trunc" ? dst.integer_bits() > src.integer_bits()
			                 : dst.integer_bits() < src.integer_bits())
				fail("invalid integer conversion widths: " + op);
		}
		else if(op == "sitofp" || op == "uitofp")
		{
			if(!src.is_integer() || !dst.is_float())
				fail(op + " requires integer source and float result");
		}
		else if(op == "fptosi" || op == "fptoui")
		{
			if(!src.is_float() || !dst.is_integer())
				fail(op + " requires float source and integer result");
		}
		else if(op == "fpext" || op == "fptrunc")
		{
			if(!src.is_float() || !dst.is_float())
				fail(op + " requires float types");
			if(op == "fpext" ? dst.operand_bits() <= src.operand_bits()
			                 : dst.operand_bits() >= src.operand_bits())
				fail("invalid float conversion widths: " + op);
		}
		else
			fail("unknown conversion operator: " + op);
	}

	void check_operator(const LowIRInstruction & ins)
	{
		static const char * const kUnaries[] = {"neg", "not", "bitnot",
			"bswap", "decay", nullptr};
		static const char * const kBinaries[] = {"add", "sub", "mul",
			"div", "mod", "udiv", "umod", "and", "or", "xor", "shl",
			"shr", "ushr", nullptr};
		static const char * const kPredicates[] = {"eq", "ne", "lt", "le",
			"gt", "ge", "ult", "ule", "ugt", "uge", nullptr};
		if(ins.opcode == LOWIR_INS_UNARY)
		{
			if(!value_in(ins.operation, kUnaries))
				fail("unknown unary operator: " + ins.operation);
			if(ins.operation == "decay" &&
			   ins.type.kind != LOWIR_TYPE_PTR)
				fail("unary decay requires ptr");
			if(ins.operation == "bswap" &&
			   !(ins.type.is_integer() && ins.type.integer_bits() >= 16))
				fail("unary bswap requires i16, i32, or i64");
		}
		else if(ins.opcode == LOWIR_INS_BINARY)
		{
			if(!value_in(ins.operation, kBinaries))
				fail("unknown binary operator: " + ins.operation);
		}
		else if(ins.opcode == LOWIR_INS_CMP)
		{
			if(!value_in(ins.operation, kPredicates))
				fail("unknown compare predicate: " + ins.operation);
		}
	}

	void check_index_metadata(const LowIRInstruction & ins)
	{
		for(size_t i = 0; i < ins.metadata.items.size(); ++i)
		{
			const string & key = ins.metadata.items[i].first;
			const string & value = ins.metadata.items[i].second;
			if(key != "projection")
				fail("unknown index metadata key: " + key);
			if(!value_in(value, kProjections))
				fail("unknown projection value: " + value);
		}
	}

	void check_span(const LowIRInstruction & ins)
	{
		if(ins.span_bytes <= 0)
			fail("bulk memory operation requires positive bytes");
		if(ins.span_align <= 0 ||
		   (ins.span_align & (ins.span_align - 1)) != 0)
			fail("bulk memory alignment must be a power of two");
	}

	void check_call(const LowIRInstruction & ins)
	{
		if(ins.signature.present)
			check_signature_boundary(ins.signature);
		if(ins.callee_is_temp)
		{
			if(!ins.signature.present)
				fail("indirect call requires an explicit signature");
			return;
		}
		map<string, const LowIRFunction *>::const_iterator found =
			info.functions.find(ins.callee);
		if(found == info.functions.end())
		{
			if(!info.globals.count(ins.callee))
				fail("call target is not declared: @" + ins.callee);
			if(!ins.signature.present)
				fail("indirect call requires an explicit signature");
			return;
		}
		const LowIRFunction & target = *found->second;
		string arity = target.metadata.find("arity");
		size_t given = ins.operands.size();
		size_t declared = target.params.size();
		if(arity == "variadic" || arity == "prototype_relaxed")
		{
			if(given < declared)
				fail("too few arguments to @" + ins.callee);
		}
		else if(given != declared)
			fail("argument count mismatch calling @" + ins.callee);
	}

	void check_operand_symbols(const LowIRFunction & function,
	                           const LowIROperand & operand,
	                           const set<string> & slots,
	                           const set<string> & temps)
	{
		if(operand.kind == LOWIR_OPERAND_TEMP && !temps.count(operand.name))
			fail("use of undefined temporary %" + operand.name + " in @" +
			     function.name);
		if(operand.kind == LOWIR_OPERAND_SLOT && !slots.count(operand.name))
			fail("use of undefined slot $" + operand.name + " in @" +
			     function.name);
		if(operand.kind == LOWIR_OPERAND_GLOBAL &&
		   !symbol_exists(operand.name))
			fail("use of undeclared symbol @" + operand.name + " in @" +
			     function.name);
	}

	void check_block_structure(const LowIRFunction & function,
	                           const LowIRBlock & block,
	                           const set<string> & labels)
	{
		if(block.instructions.empty() ||
		   !block.instructions.back().is_terminator())
			fail("block ^" + block.label + " in @" + function.name +
			     " does not end in a terminator");
		for(size_t i = 0; i + 1 < block.instructions.size(); ++i)
			if(block.instructions[i].is_terminator())
				fail("instruction after terminator in ^" + block.label +
				     " of @" + function.name);
		for(size_t i = 0; i < block.instructions.size(); ++i)
		{
			const LowIRInstruction & ins = block.instructions[i];
			for(size_t t = 0; t < ins.block_targets.size(); ++t)
				if(!labels.count(ins.block_targets[t]))
					fail("unknown block target ^" + ins.block_targets[t] +
					     " in @" + function.name);
		}
	}

	void check_instruction(const LowIRFunction & function,
	                       const LowIRInstruction & ins,
	                       const set<string> & slots, set<string> & temps)
	{
		for(size_t i = 0; i < ins.operands.size(); ++i)
			check_operand_symbols(function, ins.operands[i], slots, temps);
		for(size_t i = 0; i < ins.switch_values.size(); ++i)
			check_operand_symbols(function, ins.switch_values[i], slots,
			                      temps);
		switch(ins.opcode)
		{
		case LOWIR_INS_CONVERT:
			check_convert(ins);
			break;
		case LOWIR_INS_UNARY:
		case LOWIR_INS_BINARY:
		case LOWIR_INS_CMP:
			check_operator(ins);
			break;
		case LOWIR_INS_INDEX:
			check_index_metadata(ins);
			if(ins.type.is_void())
				fail("index requires a sized element type");
			break;
		case LOWIR_INS_COPYOBJ:
		case LOWIR_INS_ZEROINIT:
			check_span(ins);
			break;
		case LOWIR_INS_CALL:
			if(ins.callee_is_temp && !temps.count(ins.callee))
				fail("use of undefined temporary %" + ins.callee +
				     " in @" + function.name);
			check_call(ins);
			break;
		case LOWIR_INS_EXCEPTION:
		case LOWIR_INS_THROW:
			if(ins.type.is_void())
				fail("exception payloads must be non-void");
			break;
		default:
			break;
		}
		if(!ins.result.empty())
			temps.insert(ins.result);
	}

	void check_function(const LowIRFunction & function)
	{
		check_function_metadata(function);
		set<string> temps;
		for(size_t i = 0; i < function.params.size(); ++i)
			if(!temps.insert(function.params[i].name).second)
				fail("duplicate parameter %" + function.params[i].name +
				     " in @" + function.name);
		if(!function.is_definition)
			return;
		set<string> slots;
		for(size_t i = 0; i < function.slots.size(); ++i)
			if(!slots.insert(function.slots[i].name).second)
				fail("duplicate slot $" + function.slots[i].name +
				     " in @" + function.name);
		if(function.blocks.empty())
			fail("function @" + function.name + " has no blocks");
		set<string> labels;
		for(size_t i = 0; i < function.blocks.size(); ++i)
			if(!labels.insert(function.blocks[i].label).second)
				fail("duplicate block ^" + function.blocks[i].label +
				     " in @" + function.name);
		for(size_t i = 0; i < function.blocks.size(); ++i)
			check_block_structure(function, function.blocks[i], labels);
		for(size_t b = 0; b < function.blocks.size(); ++b)
			for(size_t i = 0; i < function.blocks[b].instructions.size();
			    ++i)
				check_instruction(function,
				                  function.blocks[b].instructions[i],
				                  slots, temps);
	}

	// Hook resolution: explicit roles win; the legacy spellings apply
	// only when no symbol claims the corresponding role.
	string find_hook(const string & role, const string & legacy)
	{
		for(size_t i = 0; i < program.functions.size(); ++i)
		{
			const LowIRFunction & function = program.functions[i];
			if(function.metadata.find("role") == role)
			{
				if(!function.is_definition)
					fail("hook role " + role + " requires a definition");
				return function.name;
			}
		}
		map<string, const LowIRFunction *>::const_iterator found =
			info.functions.find(legacy);
		if(found != info.functions.end() && found->second->is_definition &&
		   !found->second->metadata.has("role"))
			return found->second->name;
		return "";
	}

	void resolve_hooks()
	{
		info.entry_function = find_hook("entry", "main");
		info.init_function = find_hook("init", "__cppgm_init");
		info.fini_function = find_hook("fini", "__cppgm_fini");
		if(require_entry && info.entry_function.empty())
			fail("program has no entry function");
	}

	LowIRProgramInfo run()
	{
		check_symbols();
		check_aliases();
		for(size_t i = 0; i < program.globals.size(); ++i)
			check_global(program.globals[i]);
		for(size_t i = 0; i < program.functions.size(); ++i)
			check_function(program.functions[i]);
		resolve_hooks();
		return info;
	}
};

}  // namespace

LowIRProgramInfo ValidateLowIRProgram(const LowIRProgram & program,
                                      bool require_entry)
{
	Validator validator(program, require_entry);
	return validator.run();
}
