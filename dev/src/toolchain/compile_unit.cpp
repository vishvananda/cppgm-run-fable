#include "toolchain/compile_unit.h"

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

int SymbolIndexForFunction(const ObjectModule & module,
                           const vector<string> & label_names,
                           const string & low_name)
{
	if (low_name.empty())
		return -1;
	for (size_t i = 0; i < label_names.size(); i++)
		if (label_names[i] == low_name && module.symbols[i].item >= 0)
			return static_cast<int>(i);
	return -1;
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
			symbol.external_name =
				symbol.binding == ObjectSymbol::SB_INTERNAL
					? string()
					: ExternalNameFromMetadata(*metadata, low_name);
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

	// `alias object X = @y`: X is a second external name of y's item.
	for (size_t i = 0; i < program.aliases.size(); i++)
	{
		const LowIRAlias & alias = program.aliases[i];
		int target_symbol =
			SymbolIndexForFunction(module, native.label_names,
			                       alias.target);
		if (target_symbol < 0)
			continue;  // alias of an unlowered (undemanded) definition
		ObjectSymbol symbol;
		symbol.external_name = alias.object_symbol;
		symbol.binding =
			module.symbols[target_symbol].binding == ObjectSymbol::SB_WEAK
				? ObjectSymbol::SB_WEAK
				: ObjectSymbol::SB_STRONG;
		symbol.item = module.symbols[target_symbol].item;
		symbol.offset = module.symbols[target_symbol].offset;
		module.symbols.push_back(symbol);
	}

	module.entry_symbol =
		SymbolIndexForFunction(module, native.label_names,
		                       info.entry_function);
	module.init_symbol =
		SymbolIndexForFunction(module, native.label_names,
		                       info.init_function);
	module.fini_symbol =
		SymbolIndexForFunction(module, native.label_names,
		                       info.fini_function);
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
