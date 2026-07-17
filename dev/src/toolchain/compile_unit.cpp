#include "toolchain/compile_unit.h"

#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <pthread.h>

#include "ast/ast.h"
#include "ast/ast_parser.h"
#include "lowering/lower_program.h"
#include "lowir/lowir_parser.h"
#include "lowir/lowir_validate.h"
#include "parse/parse_token.h"
#include "post_token.h"
#include "post_tokenizer.h"
#include "predefined_macros.h"
#include "preprocess.h"
#include "sema/scope.h"
#include "sema/sem_binder.h"
#include "sema/sem_node.h"
#include "toolchain/runtime_library.h"
#include "x86/frame_cfi.h"
#include "x86/lowir_to_mir.h"
#include "x86/mir_to_native.h"

using std::runtime_error;
using std::string;
using std::vector;

namespace toolchain {

namespace {

// Collects the phase-7 tokens of one srcfile for the parser; invalid
// tokens are kept and rejected by BuildParseTokens.
struct CollectingPostTokenStream : IPostTokenStream
{
	void emit(const PostToken & token)
	{
		tokens.push_back(token);
	}

	vector<PostToken> tokens;
};

// Recursive descent depth is proportional to input nesting, so each
// unit compiles through LowIR emission on a worker thread with a large
// stack (virtual memory; pages commit only as touched).
const size_t kCompileStackBytes = 512u << 20;

struct CompileUnitTask
{
	const string * presumed_name;
	const string * text;  // null: read presumed_name from disk
	const CompileOptions * options;
	string lowir_text;
	bool failed;
	string message;
};

void * compile_unit_thread_main(void * opaque)
{
	CompileUnitTask * task = static_cast<CompileUnitTask *>(opaque);
	try
	{
		CollectingPostTokenStream collector;
		PostTokenizer post_tokenizer(collector);
		Preprocessor preprocessor(post_tokenizer,
		                          PredefinedObjectMacros());
		for (size_t i = 0; i < task->options->include_dirs.size(); i++)
			preprocessor.AddIncludeDir(task->options->include_dirs[i]);
		if (task->text)
			preprocessor.ProcessSourceText(*task->presumed_name,
			                               *task->text);
		else
			preprocessor.ProcessSourceFile(*task->presumed_name);

		vector<ParseToken> tokens = BuildParseTokens(collector.tokens);
		AstParser parser(tokens);
		AstDeclPtr unit = parser.ParseTranslationUnit();

		TypesModel model;
		SemUnit semantics;
		SemBinder binder(model, semantics);
		binder.BindTranslationUnit(*unit);

		LowerProgram lowering;
		lowering.SetSeparateCompilation();
		lowering.AddUnit(semantics);
		std::ostringstream lowir;
		lowering.Write(lowir);
		task->lowir_text = lowir.str();
	}
	catch (const std::exception & e)
	{
		task->failed = true;
		task->message = e.what();
	}
	return 0;
}

string LowerUnitToLowIR(const string & presumed_name, const string * text,
                        const CompileOptions & options)
{
	CompileUnitTask task;
	task.presumed_name = &presumed_name;
	task.text = text;
	task.options = &options;
	task.failed = false;
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) != 0 ||
	    pthread_attr_setstacksize(&attributes, kCompileStackBytes) != 0 ||
	    pthread_create(&thread, &attributes, compile_unit_thread_main,
	                   &task) != 0)
		throw runtime_error("cannot start compile worker thread");
	pthread_join(thread, 0);
	pthread_attr_destroy(&attributes);
	if (task.failed)
		throw runtime_error(task.message);
	return task.lowir_text;
}

ObjectSymbol::Binding BindingFromMetadata(const LowIRMetadata & metadata,
                                          bool is_definition)
{
	if (!is_definition)
		return ObjectSymbol::SB_UNDEFINED;
	string binding = metadata.find("binding");
	if (binding == "internal")
		return ObjectSymbol::SB_INTERNAL;
	if (binding == "weak")
		return ObjectSymbol::SB_WEAK;
	return ObjectSymbol::SB_STRONG;
}

// The cross-module resolution key: the `object=` spelling when it is
// a real external name; `object=@low` self-merge keys and missing
// object names fall back to the low name.
string ExternalNameFromMetadata(const LowIRMetadata & metadata,
                                const string & low_name)
{
	string object_name = metadata.find("object");
	if (object_name.empty() || object_name[0] == '@')
		return low_name;
	return object_name;
}

// Defined low name -> symbol index (first definition wins), so alias
// targets and role hooks resolve without rescanning the label space.
std::map<string, int> BuildDefinedSymbolIndex(
	const ObjectModule & module, const vector<string> & label_names)
{
	std::map<string, int> index;
	for (size_t i = 0; i < label_names.size(); i++)
		if (!label_names[i].empty() && module.symbols[i].item >= 0)
			index.insert(std::make_pair(label_names[i],
			                            static_cast<int>(i)));
	return index;
}

int FindDefinedSymbol(const std::map<string, int> & index,
                      const string & low_name)
{
	std::map<string, int>::const_iterator found = index.find(low_name);
	return found == index.end() ? -1 : found->second;
}

// Catch-clause type symbols may be referenced only by classification
// markers (no code), so they can be missing from the encoder's label
// space; append them to the symbol table on demand with their declared
// binding and external name.
int FindOrAddTypeSymbol(ObjectModule & module,
                        std::map<string, int> & by_low_name,
                        const LowIRProgramInfo & info,
                        const string & low_name)
{
	std::map<string, int>::const_iterator found =
		by_low_name.find(low_name);
	if (found != by_low_name.end())
		return found->second;
	std::map<string, const LowIRGlobal *>::const_iterator global =
		info.globals.find(low_name);
	if (global == info.globals.end())
		throw runtime_error("eh catch type is not a global: " + low_name);
	ObjectSymbol symbol;
	symbol.low_name = low_name;
	symbol.binding = BindingFromMetadata(global->second->metadata,
	                                     global->second->is_definition);
	symbol.external_name =
		symbol.binding == ObjectSymbol::SB_INTERNAL
			? string()
			: ExternalNameFromMetadata(global->second->metadata, low_name);
	module.symbols.push_back(symbol);
	by_low_name[low_name] = static_cast<int>(module.symbols.size()) - 1;
	return static_cast<int>(module.symbols.size()) - 1;
}

// The action chain of one region: its landing pad's catch clauses,
// a cleanup record for eh_cleanup armings, then the enclosing
// regions' actions (innermost-first personality scan order).
vector<EhAction> BuildEhChain(ObjectModule & module,
                              std::map<string, int> & by_low_name,
                              const LowIRProgramInfo & info,
                              const NativeFunctionEh & fn, int region)
{
	vector<EhAction> chain;
	for (int r = region; r >= 0; r = fn.regions[static_cast<size_t>(r)].parent)
	{
		const NativeEhRegion & q = fn.regions[static_cast<size_t>(r)];
		for (size_t c = 0; c < q.clauses.size(); c++)
		{
			const mir_model::HostEhClause & clause = q.clauses[c];
			EhAction action;
			action.filter = clause.selector;
			if (clause.catch_all)
			{
				action.kind = EhAction::EH_CATCH_ALL;
			}
			else
			{
				action.kind = EhAction::EH_CATCH;
				action.type_symbol = FindOrAddTypeSymbol(
					module, by_low_name, info, clause.type_symbol);
			}
			chain.push_back(action);
		}
		if (q.cleanup)
		{
			EhAction action;
			action.kind = EhAction::EH_CLEANUP;
			chain.push_back(action);
		}
	}
	return chain;
}

// PA31: the host runtime owns fundamental typeinfo (libstdc++ exports
// it; the private runtime module defines the same set), so weak
// fundamental _ZTI/_ZTS definitions demote to undefined references
// and their data items vanish from the emitted object.
void DemoteRuntimeOwnedTypeinfo(ObjectModule & module)
{
	for (size_t s = 0; s < module.symbols.size(); s++)
	{
		ObjectSymbol & symbol = module.symbols[s];
		if (symbol.item < 0 ||
		    symbol.binding != ObjectSymbol::SB_WEAK ||
		    !IsRuntimeOwnedTypeinfoName(symbol.external_name))
			continue;
		ImageItem & item =
			module.items[static_cast<size_t>(symbol.item)];
		item.bytes.clear();
		item.patches.clear();
		symbol.item = -1;
		symbol.offset = 0;
		symbol.binding = ObjectSymbol::SB_UNDEFINED;
	}
}

// Converts the encoder's per-function EH facts into the module's typed
// form: call-site rows, memoized action chains, and the CFI stream.
void AppendEhFunctionFacts(ObjectModule & module,
                           const LowIRProgramInfo & info,
                           const NativeModule & native)
{
	std::map<string, int> by_low_name;
	for (size_t i = 0; i < module.symbols.size(); i++)
		if (!module.symbols[i].low_name.empty())
			by_low_name.insert(std::make_pair(module.symbols[i].low_name,
			                                  static_cast<int>(i)));
	for (size_t f = 0; f < native.function_eh.size(); f++)
	{
		const NativeFunctionEh & fn = native.function_eh[f];
		EhFunctionInfo eh;
		eh.item = fn.item;
		eh.code_size = fn.code_size;
		eh.cfi_ops = BuildFrameCfiOps(fn);
		std::map<int, int> chain_of_region;
		for (size_t s = 0; s < fn.call_sites.size(); s++)
		{
			const NativeEhCallSite & site = fn.call_sites[s];
			std::map<int, int>::const_iterator known =
				chain_of_region.find(site.region);
			int chain;
			if (known != chain_of_region.end())
			{
				chain = known->second;
			}
			else
			{
				eh.chains.push_back(BuildEhChain(module, by_low_name,
				                                 info, fn, site.region));
				chain = static_cast<int>(eh.chains.size()) - 1;
				chain_of_region[site.region] = chain;
			}
			EhCallSite row;
			row.start = site.start;
			row.length = site.end - site.start;
			row.landing_pad =
				fn.regions[static_cast<size_t>(site.region)].landing_offset;
			row.chain = chain;
			eh.call_sites.push_back(row);
		}
		module.eh_functions.push_back(eh);
	}
}

// Builds the object symbol table over the encoder's label space (one
// symbol per label, indices preserved so patches stay valid), then
// appends alias names and resolves the role hooks.
ObjectModule BuildObjectModule(const LowIRProgram & program,
                               const LowIRProgramInfo & info,
                               const NativeModule & native,
                               const string & target)
{
	ObjectModule module;
	module.target = target;
	module.items = native.items;

	for (size_t i = 0; i < native.label_names.size(); i++)
	{
		const string & low_name = native.label_names[i];
		ObjectSymbol symbol;
		symbol.low_name = low_name;
		symbol.item = native.label_items[i];
		if (low_name.empty())
		{
			// pool constant: module-private, never resolved by name
			symbol.binding = ObjectSymbol::SB_INTERNAL;
			module.symbols.push_back(symbol);
			continue;
		}
		const LowIRMetadata * metadata = 0;
		bool is_definition = false;
		std::map<string, const LowIRFunction *>::const_iterator function =
			info.functions.find(low_name);
		if (function != info.functions.end())
		{
			metadata = &function->second->metadata;
			is_definition = function->second->is_definition;
		}
		else
		{
			std::map<string, const LowIRGlobal *>::const_iterator global =
				info.globals.find(low_name);
			if (global != info.globals.end())
			{
				metadata = &global->second->metadata;
				is_definition = global->second->is_definition;
			}
		}
		if (metadata)
		{
			symbol.binding = BindingFromMetadata(*metadata, is_definition);
			if (symbol.binding == ObjectSymbol::SB_INTERNAL)
			{
				// Internal definitions keep their ABI spelling as a
				// host-visible LOCAL name (never a resolution key).
				string object_name = metadata->find("object");
				if (!object_name.empty() && object_name[0] != '@')
					symbol.local_name = object_name;
			}
			else
			{
				symbol.external_name =
					ExternalNameFromMetadata(*metadata, low_name);
			}
			symbol.thread_local_symbol =
				metadata->find("storage") == "thread_local";
		}
		else
		{
			// Backend-generated reference (runtime hooks): the low
			// name is the external contract.
			symbol.binding = ObjectSymbol::SB_UNDEFINED;
			symbol.external_name = low_name;
		}
		module.symbols.push_back(symbol);
	}

	// PA32 host TLS: synthesized wrappers define their declared
	// symbol (weak for the exported _ZTW spelling, module-private for
	// internal thread-locals); the init probes bind weak undefined.
	for (size_t w = 0; w < native.tls_wrapper_labels.size(); w++)
		for (size_t s = 0; s < module.symbols.size(); s++)
		{
			ObjectSymbol & symbol = module.symbols[s];
			if (symbol.low_name != native.tls_wrapper_labels[w] ||
			    symbol.item < 0)
				continue;
			symbol.binding = symbol.external_name.empty() ||
				symbol.external_name == symbol.low_name
				? ObjectSymbol::SB_INTERNAL
				: ObjectSymbol::SB_WEAK;
			if (symbol.binding == ObjectSymbol::SB_INTERNAL)
				symbol.external_name.clear();
		}
	for (size_t w = 0; w < native.weak_undefined_labels.size(); w++)
		for (size_t s = 0; s < module.symbols.size(); s++)
			if (module.symbols[s].low_name ==
			        native.weak_undefined_labels[w] &&
			    module.symbols[s].item < 0)
				module.symbols[s].weak_undefined = true;

	std::map<string, int> defined =
		BuildDefinedSymbolIndex(module, native.label_names);

	// `alias object X = @y`: X is a second external name of y's item.
	for (size_t i = 0; i < program.aliases.size(); i++)
	{
		const LowIRAlias & alias = program.aliases[i];
		int target_symbol = FindDefinedSymbol(defined, alias.target);
		if (target_symbol < 0)
			continue;  // alias of an unlowered (undemanded) definition
		ObjectSymbol symbol;
		if (module.symbols[target_symbol].binding ==
		    ObjectSymbol::SB_INTERNAL)
		{
			// An alias of an internal definition is a second local
			// spelling, never an exported name.
			symbol.local_name = alias.object_symbol;
			symbol.binding = ObjectSymbol::SB_INTERNAL;
		}
		else
		{
			symbol.external_name = alias.object_symbol;
			symbol.binding =
				module.symbols[target_symbol].binding ==
					ObjectSymbol::SB_WEAK
					? ObjectSymbol::SB_WEAK
					: ObjectSymbol::SB_STRONG;
		}
		symbol.item = module.symbols[target_symbol].item;
		symbol.offset = module.symbols[target_symbol].offset;
		module.symbols.push_back(symbol);
	}

	module.entry_symbol = FindDefinedSymbol(defined, info.entry_function);
	module.init_symbol = FindDefinedSymbol(defined, info.init_function);
	module.fini_symbol = FindDefinedSymbol(defined, info.fini_function);
	AppendEhFunctionFacts(module, info, native);
	DemoteRuntimeOwnedTypeinfo(module);
	return module;
}

ObjectModule CompileToModule(const string & presumed_name,
                             const string * text,
                             const CompileOptions & options)
{
	string lowir_text = LowerUnitToLowIR(presumed_name, text, options);
	LowIRProgram program = ParseLowIRProgram(lowir_text);
	LowIRProgramInfo info = ValidateLowIRProgram(program, false);
	string target = options.target.empty() ? "linux" : options.target;
	mir_model::MirProgram machine_ir =
		LowerLowIRProgramToMir(program, info, target);
	// PA32: `-c` objects carry real host TLS (STT_TLS storage and the
	// per-TU _ZTW wrapper).
	machine_ir.host_tls = true;
	NativeModule native = EncodeMirProgramModule(machine_ir);
	return BuildObjectModule(program, info, native, target);
}

}  // namespace

ObjectModule CompileSourceFileToModule(const string & path,
                                       const CompileOptions & options)
{
	return CompileToModule(path, 0, options);
}

ObjectModule CompileSourceTextToModule(const string & presumed_name,
                                       const string & text,
                                       const CompileOptions & options)
{
	return CompileToModule(presumed_name, &text, options);
}

}  // namespace toolchain
