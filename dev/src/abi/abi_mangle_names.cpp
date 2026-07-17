// Qualified names, function encodings, special ABI names, and the per-case
// mangling driver for the PA30 abimangle tool.

#include "abi/abi_mangle_encode.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace std;

namespace abi_mangle {

const AbiType & CaseEnvironment::type(const string & id) const
{
  map<string, const AbiType *>::const_iterator it = types.find(id);
  if(it == types.end()) {
    throw AbiFactError("unknown type fact '" + id + "'");
  }
  return *it->second;
}

const AbiTemplateArgument & CaseEnvironment::argument(const string & id) const
{
  map<string, const AbiTemplateArgument *>::const_iterator it = arguments.find(id);
  if(it == arguments.end()) {
    throw AbiFactError("unknown template argument fact '" + id + "'");
  }
  return *it->second;
}

const AbiDependentExpression & CaseEnvironment::expression(const string & id) const
{
  map<string, const AbiDependentExpression *>::const_iterator it = expressions.find(id);
  if(it == expressions.end()) {
    throw AbiFactError("unknown expression fact '" + id + "'");
  }
  return *it->second;
}

const AbiLocalContext & CaseEnvironment::context(const string & id) const
{
  map<string, const AbiLocalContext *>::const_iterator it = contexts.find(id);
  if(it == contexts.end()) {
    throw AbiFactError("unknown context fact '" + id + "'");
  }
  return *it->second;
}

const AbiEntityFact & CaseEnvironment::entity(const string & id) const
{
  map<string, const AbiEntityFact *>::const_iterator it = entities.find(id);
  if(it == entities.end()) {
    throw AbiFactError("unknown entity fact '" + id + "'");
  }
  return *it->second;
}

CaseEnvironment build_case_environment(const AbiFactCase & fact_case)
{
  CaseEnvironment env;
  for(size_t i = 0; i < fact_case.records.size(); ++i) {
    const AbiFactRecord & record = fact_case.records[i];
    if(record.kind != ABI_FACT_RECORD_DEFINITION) {
      continue;
    }
    const AbiDefinitionRecord & def = record.definition;
    switch(def.kind) {
    case ABI_DEFINITION_TYPE:
      env.types[def.id] = &def.type;
      break;
    case ABI_DEFINITION_TEMPLATE_ARGUMENT:
      env.arguments[def.id] = &def.template_argument;
      break;
    case ABI_DEFINITION_EXPRESSION:
      env.expressions[def.id] = &def.expression;
      break;
    case ABI_DEFINITION_CONTEXT:
      env.contexts[def.id] = &def.context;
      break;
    case ABI_DEFINITION_ENTITY:
      env.entities[def.id] = &def.entity;
      break;
    }
  }
  return env;
}

namespace {

string decimal(long long value)
{
  ostringstream out;
  out << value;
  return out.str();
}

string offset_number(long long value)
{
  return value < 0 ? "n" + decimal(-value) : decimal(value);
}

struct OperatorEntry
{
  const char * name;
  const char * code;
  const char * unary_code;
};

const OperatorEntry * operator_table_lookup(const string & name)
{
  static const OperatorEntry table[] = {
    {"new", "nw", 0},          {"new-array", "na", 0},
    {"delete", "dl", 0},       {"delete-array", "da", 0},
    {"plus", "pl", "ps"},      {"minus", "mi", "ng"},
    {"unary-plus", "ps", 0},   {"binary-plus", "pl", 0},
    {"unary-minus", "ng", 0},  {"binary-minus", "mi", 0},
    {"multiply", "ml", 0},     {"divide", "dv", 0},
    {"remainder", "rm", 0},    {"deref", "de", 0},
    {"address-of", "ad", 0},   {"bit-and", "an", 0},
    {"bit-or", "or", 0},       {"bit-xor", "eo", 0},
    {"complement", "co", 0},   {"assign", "aS", 0},
    {"plus-assign", "pL", 0},  {"minus-assign", "mI", 0},
    {"multiply-assign", "mL", 0}, {"divide-assign", "dV", 0},
    {"remainder-assign", "rM", 0}, {"and-assign", "aN", 0},
    {"or-assign", "oR", 0},    {"xor-assign", "eO", 0},
    {"left-shift", "ls", 0},   {"right-shift", "rs", 0},
    {"shift-left", "ls", 0},   {"shift-right", "rs", 0},
    {"left-shift-assign", "lS", 0}, {"right-shift-assign", "rS", 0},
    {"shift-left-assign", "lS", 0}, {"shift-right-assign", "rS", 0},
    {"equal", "eq", 0},        {"not-equal", "ne", 0},
    {"less", "lt", 0},         {"greater", "gt", 0},
    {"less-equal", "le", 0},   {"greater-equal", "ge", 0},
    {"spaceship", "ss", 0},    {"logical-and", "aa", 0},
    {"logical-or", "oo", 0},   {"logical-not", "nt", 0},
    {"not", "nt", 0},          {"increment", "pp", 0},
    {"decrement", "mm", 0},    {"comma", "cm", 0},
    {"member-pointer", "pm", 0}, {"arrow", "pt", 0},
    {"call", "cl", 0},         {"operator-call", "cl", 0},
    {"index", "ix", 0},
  };
  for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
    if(name == table[i].name) {
      return &table[i];
    }
  }
  return 0;
}

string operator_terminal_code(const string & name, size_t operand_count)
{
  const OperatorEntry * entry = operator_table_lookup(name);
  if(entry == 0) {
    throw AbiFactError("unknown operator terminal '" + name + "'");
  }
  if(entry->unary_code != 0 && operand_count == 1) {
    return entry->unary_code;
  }
  return entry->code;
}

string special_terminal_code(const string & name)
{
  static const struct { const char * name; const char * code; } table[] = {
    {"constructor-complete", "C1"},   {"constructor-base", "C2"},
    {"constructor-allocating", "C3"}, {"destructor-deleting", "D0"},
    {"destructor-complete", "D1"},    {"destructor-base", "D2"},
  };
  for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
    if(name == table[i].name) {
      return table[i].code;
    }
  }
  throw AbiFactError("unknown special terminal '" + name + "'");
}

// Function detail records collected from a case, in fact order.
struct FunctionDetails
{
  vector<const AbiFunctionRecord *> names;
  const AbiFunctionRecord * local_context = 0;
  const AbiFunctionRecord * lambda_context = 0;
  const AbiFunctionRecord * terminal = 0;
  vector<const AbiType *> parameters;
  const AbiType * result = 0;
  bool variadic = false;
  vector<string> abi_tags;
  vector<string> template_argument_refs;
  string template_prefix_key;
  bool qual_const = false;
  bool qual_volatile = false;
  bool qual_lvalue_ref = false;
  bool qual_rvalue_ref = false;
};

void collect_function_detail(FunctionDetails & details, const AbiFunctionRecord & record)
{
  switch(record.kind) {
  case ABI_FUNCTION_RECORD_NAME_SOURCE:
  case ABI_FUNCTION_RECORD_NAME_STD:
  case ABI_FUNCTION_RECORD_NAME_TEMPLATE:
    details.names.push_back(&record);
    break;
  case ABI_FUNCTION_RECORD_LOCAL_CONTEXT:
    details.local_context = &record;
    break;
  case ABI_FUNCTION_RECORD_LAMBDA_CONTEXT:
    details.lambda_context = &record;
    break;
  case ABI_FUNCTION_RECORD_TERMINAL:
  case ABI_FUNCTION_RECORD_TERMINAL_SOURCE:
  case ABI_FUNCTION_RECORD_OPERATOR_TERMINAL:
  case ABI_FUNCTION_RECORD_CONVERSION_TERMINAL:
    details.terminal = &record;
    break;
  case ABI_FUNCTION_RECORD_PARAMETER:
    details.parameters.push_back(&record.type);
    break;
  case ABI_FUNCTION_RECORD_RESULT:
    details.result = &record.type;
    break;
  case ABI_FUNCTION_RECORD_VARIADIC:
    details.variadic = true;
    break;
  case ABI_FUNCTION_RECORD_ABI_TAG:
    details.abi_tags.push_back(record.name);
    break;
  case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT:
    details.template_argument_refs.insert(details.template_argument_refs.end(),
                                          record.argument_refs.begin(), record.argument_refs.end());
    break;
  case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX:
    details.template_prefix_key = record.substitution;
    break;
  case ABI_FUNCTION_RECORD_QUALIFIER:
    for(size_t i = 0; i < record.qualifiers.size(); ++i) {
      switch(record.qualifiers[i]) {
      case ABI_FUNCTION_QUALIFIER_CONST:
        details.qual_const = true;
        break;
      case ABI_FUNCTION_QUALIFIER_VOLATILE:
        details.qual_volatile = true;
        break;
      case ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE:
        details.qual_lvalue_ref = true;
        break;
      case ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE:
        details.qual_rvalue_ref = true;
        break;
      }
    }
    break;
  }
}

string explicit_key(const string & key)
{
  return key == "-" ? string() : key;
}

// Assembled name pieces of one function encoding, before signature emission.
struct FunctionName
{
  string context_piece;
  vector<NameLink> links;
  bool has_terminal_slot = false;
  size_t terminal_slot = 0;
  string terminal_slot_key;
  string discriminator_suffix;
  vector<string> template_argument_refs;
  vector<const AbiType *> parameters;
};

// Appends the link for one name record; an empty name-source is a terminal
// slot marker instead of a component of its own.
void append_name_record_link(const AbiFunctionRecord & record, FunctionName & name)
{
  NameLink link;
  if(record.kind == ABI_FUNCTION_RECORD_NAME_STD) {
    link.spelled = "St";
    link.std_abbrev = true;
    name.links.push_back(link);
    return;
  }
  if(record.kind == ABI_FUNCTION_RECORD_NAME_SOURCE) {
    if(record.name.empty()) {
      name.has_terminal_slot = true;
      name.terminal_slot = name.links.size();
      name.terminal_slot_key = explicit_key(record.substitution);
      return;
    }
    link.spelled = source_name(record.name);
    link.key = explicit_key(record.substitution);
    name.links.push_back(link);
    return;
  }
  const string std_code = explicit_key(record.standard_substitution);
  if(!std_code.empty()) {
    link.spelled = std_code;
    if(record.standard_substitution_includes_arguments) {
      name.links.push_back(link);
      return;
    }
  } else {
    link.spelled = source_name(record.name);
  }
  link.key = explicit_key(record.substitution);
  link.has_args = true;
  link.argument_refs = record.argument_refs;
  link.key_with_args = explicit_key(record.complete_substitution);
  name.links.push_back(link);
}

void build_path_name(const AbiFunctionTarget & target, FunctionName & name)
{
  name.links = qualified_name_links(target.qualified_name, true);
  const string & path = target.qualified_name;
  const bool operator_placeholder =
      path == "operator" || (path.size() > 10 && path.compare(path.size() - 10, 10, "::operator") == 0);
  if(operator_placeholder) {
    name.links.pop_back();
    name.has_terminal_slot = true;
    name.terminal_slot = name.links.size();
  }
  for(size_t i = 0; i < target.path_operands.size(); ++i) {
    const AbiFunctionPathOperand & operand = target.path_operands[i];
    switch(operand.kind) {
    case ABI_FUNCTION_PATH_TYPE:
      name.parameters.push_back(&operand.type);
      break;
    case ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT:
      name.template_argument_refs.push_back(operand.argument_ref);
      break;
    case ABI_FUNCTION_PATH_RESULT_TYPE:
    case ABI_FUNCTION_PATH_VARIADIC:
      throw AbiFactError("unsupported function path operand");
    }
  }
}

void build_function_name(EncodeContext & ctx, const AbiFunctionTarget & target,
                         const FunctionDetails & details, FunctionName & name)
{
  switch(target.kind) {
  case ABI_FUNCTION_TARGET_PATH:
    build_path_name(target, name);
    break;
  case ABI_FUNCTION_TARGET_ENCODING:
    for(size_t i = 0; i < details.names.size(); ++i) {
      append_name_record_link(*details.names[i], name);
    }
    if(details.local_context != 0) {
      const AbiFunctionRecord & record = *details.local_context;
      name.context_piece = local_context_piece(ctx, record.context_ref);
      NameLink link;
      link.spelled = source_name(record.source_name);
      link.key = "Zt:" + record.context_ref + "::" + record.source_name + "#" + record.discriminator;
      name.links.push_back(link);
      name.discriminator_suffix = local_discriminator(record.discriminator);
    }
    if(details.lambda_context != 0) {
      const AbiFunctionRecord & record = *details.lambda_context;
      name.context_piece = local_context_piece(ctx, record.context_ref);
      NameLink link;
      link.spelled = closure_type_piece(ctx, record.discriminator, record.types);
      link.key = "Zl:" + record.context_ref + "#" + record.discriminator;
      name.links.push_back(link);
    }
    break;
  case ABI_FUNCTION_TARGET_LOCAL: {
    name.context_piece = local_context_piece(ctx, target.context_ref);
    NameLink link;
    link.spelled = source_name(target.source_name);
    link.key = "Zt:" + target.context_ref + "::" + target.source_name + "#" + target.discriminator;
    name.links.push_back(link);
    name.discriminator_suffix = local_discriminator(target.discriminator);
    break;
  }
  case ABI_FUNCTION_TARGET_LAMBDA: {
    name.context_piece = local_context_piece(ctx, target.context_ref);
    NameLink link;
    link.spelled = closure_type_piece(ctx, target.discriminator, target.signature_parameter_types);
    link.key = "Zl:" + target.context_ref + "#" + target.discriminator;
    name.links.push_back(link);
    break;
  }
  }
}

// The terminal component: an explicit terminal record, the target's semantic
// terminal name (local/lambda call operators), or the already-built last link.
void place_terminal(EncodeContext & ctx, const AbiFunctionTarget & target,
                    const FunctionDetails & details, size_t parameter_count, FunctionName & name)
{
  const bool is_member = !name.links.empty() || name.has_terminal_slot;
  const size_t operand_count = parameter_count + (is_member ? 1 : 0);
  NameLink link;
  bool have_terminal = false;
  if(details.terminal != 0) {
    const AbiFunctionRecord & record = *details.terminal;
    have_terminal = true;
    switch(record.kind) {
    case ABI_FUNCTION_RECORD_TERMINAL:
      link.spelled = special_terminal_code(record.terminal);
      break;
    case ABI_FUNCTION_RECORD_TERMINAL_SOURCE:
      link.spelled = source_name(record.source_name);
      break;
    case ABI_FUNCTION_RECORD_OPERATOR_TERMINAL:
      if(record.terminal == "literal") {
        link.spelled = "li" + source_name(record.literal_suffix);
      } else {
        link.spelled = operator_terminal_code(record.terminal, operand_count);
      }
      break;
    default:
      link.spelled = "cv";
      link.type_piece = &record.type;
      break;
    }
  } else if(!target.terminal.empty()) {
    have_terminal = true;
    if(operator_table_lookup(target.terminal) != 0) {
      link.spelled = operator_terminal_code(target.terminal, operand_count);
    } else {
      link.spelled = source_name(target.terminal);
    }
  }
  if(!have_terminal) {
    if(name.has_terminal_slot) {
      throw AbiFactError("function name has an unfilled terminal slot");
    }
    return;
  }
  if(name.has_terminal_slot) {
    link.key = name.terminal_slot_key;
    name.links.insert(name.links.begin() + name.terminal_slot, link);
  } else {
    name.links.push_back(link);
  }
}

string encode_function_encoding(EncodeContext & ctx, const AbiFunctionTarget & target,
                                const FunctionDetails & details)
{
  FunctionName name;
  build_function_name(ctx, target, details, name);
  name.template_argument_refs.insert(name.template_argument_refs.end(),
                                     details.template_argument_refs.begin(),
                                     details.template_argument_refs.end());
  name.parameters.insert(name.parameters.end(), details.parameters.begin(), details.parameters.end());
  place_terminal(ctx, target, details, name.parameters.size(), name);
  if(name.links.empty()) {
    throw AbiFactError("function target has no name");
  }
  NameLink & terminal = name.links.back();
  for(size_t i = 0; i < details.abi_tags.size(); ++i) {
    terminal.suffix += "B" + source_name(details.abi_tags[i]);
  }
  if(!name.template_argument_refs.empty()) {
    terminal.has_args = true;
    terminal.argument_refs = name.template_argument_refs;
    if(!details.template_prefix_key.empty()) {
      terminal.key = explicit_key(details.template_prefix_key);
    }
  } else if(target.kind == ABI_FUNCTION_TARGET_PATH) {
    terminal.key.clear();
  }
  terminal.key_with_args.clear();

  string qualifiers;
  if(details.qual_volatile) {
    qualifiers += "V";
  }
  if(details.qual_const) {
    qualifiers += "K";
  }
  if(details.qual_lvalue_ref) {
    qualifiers += "R";
  } else if(details.qual_rvalue_ref) {
    qualifiers += "O";
  }

  string out = name.context_piece;
  emit_name_links(ctx, name.links, qualifiers, out);
  out += name.discriminator_suffix;
  if(details.result != 0) {
    encode_type(ctx, *details.result, out);
  }
  if(name.parameters.empty() && !details.variadic) {
    out += "v";
  }
  for(size_t i = 0; i < name.parameters.size(); ++i) {
    encode_type(ctx, *name.parameters[i], out);
  }
  if(details.variadic) {
    out += "z";
  }
  return out;
}

string encode_variable_name(EncodeContext & ctx, const string & qualified_name)
{
  vector<NameLink> links = qualified_name_links(qualified_name, true);
  links.back().key.clear();
  string out;
  emit_name_links(ctx, links, "", out);
  return out;
}

}  // namespace

string local_context_piece(EncodeContext & ctx, const string & context_ref)
{
  const AbiLocalContext & context = ctx.env.context(context_ref);
  if(context.kind == ABI_CONTEXT_RAW) {
    return context.fragment;
  }
  return "Z" + encode_function_encoding(ctx, context.function, FunctionDetails()) + "E";
}

string closure_type_piece(EncodeContext & ctx, const string & number, const vector<AbiType> & signature_types)
{
  string out = "Ul";
  if(signature_types.empty()) {
    out += "v";
  }
  for(size_t i = 0; i < signature_types.size(); ++i) {
    encode_type(ctx, signature_types[i], out);
  }
  out += "E";
  if(number != "-") {
    out += number;
  }
  out += "_";
  return out;
}

string local_discriminator(const string & occurrence)
{
  if(occurrence.empty() || occurrence == "-") {
    return "";
  }
  const long long value = strtoll(occurrence.c_str(), 0, 10);
  if(value < 2) {
    return "";
  }
  return "_" + decimal(value - 2);
}

string entity_symbol(const EncodeContext & ctx, const string & entity_ref)
{
  const AbiEntityFact & entity = ctx.env.entity(entity_ref);
  if(entity.kind == ABI_ENTITY_FACT_SYMBOL) {
    return entity.qualified_name;
  }
  EncodeContext fresh(ctx.env);
  if(entity.kind == ABI_ENTITY_FACT_VARIABLE) {
    return "_Z" + encode_variable_name(fresh, entity.qualified_name);
  }
  return "_Z" + encode_function_encoding(fresh, entity.function, FunctionDetails());
}

namespace {

string mangle_function_target(EncodeContext & ctx, const AbiTargetRecord & target,
                              const FunctionDetails & details)
{
  if(target.c_linkage) {
    return target.function.qualified_name;
  }
  return "_Z" + encode_function_encoding(ctx, target.function, details);
}

string mangle_target(EncodeContext & ctx, const AbiTargetRecord & target, const FunctionDetails & details)
{
  string out;
  switch(target.kind) {
  case ABI_TARGET_FACT_TYPE:
    encode_type(ctx, target.type, out);
    return out;
  case ABI_TARGET_FACT_FUNCTION:
    return mangle_function_target(ctx, target, details);
  case ABI_TARGET_FACT_VARIABLE:
    return "_Z" + encode_variable_name(ctx, target.qualified_name);
  case ABI_TARGET_FACT_TYPEINFO:
    out = "_ZTI";
    encode_type(ctx, target.type, out);
    return out;
  case ABI_TARGET_FACT_VTABLE:
    out = "_ZTV";
    encode_type(ctx, target.type, out);
    return out;
  case ABI_TARGET_FACT_VTT:
    out = "_ZTT";
    encode_type(ctx, target.type, out);
    return out;
  case ABI_TARGET_FACT_CONSTRUCTION_VTABLE:
    out = "_ZTC";
    encode_type(ctx, target.type, out);
    out += decimal(static_cast<long long>(target.base_offset)) + "_";
    encode_type(ctx, target.base_type, out);
    return out;
  case ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER:
    return "_ZTW" + encode_variable_name(ctx, target.qualified_name);
  case ABI_TARGET_FACT_THUNK:
    if(target.has_result_adjust) {
      out = "_ZTch" + offset_number(target.this_adjust) + "_h" + offset_number(target.result_adjust) + "_";
    } else {
      out = "_ZTh" + offset_number(target.this_adjust) + "_";
    }
    return out + encode_function_encoding(ctx, target.function, details);
  case ABI_TARGET_FACT_VIRTUAL_BASE_THUNK:
    out = "_ZTv" + offset_number(target.this_adjust) + "_" + offset_number(target.vcall_offset) + "_";
    return out + encode_function_encoding(ctx, target.function, details);
  }
  throw AbiFactError("unhandled target fact kind");
}

}  // namespace

string mangle_fact_case(const AbiFactCase & fact_case)
{
  CaseEnvironment env = build_case_environment(fact_case);
  EncodeContext ctx(env);
  const AbiTargetRecord * target = 0;
  FunctionDetails details;
  for(size_t i = 0; i < fact_case.records.size(); ++i) {
    const AbiFactRecord & record = fact_case.records[i];
    if(record.kind == ABI_FACT_RECORD_TARGET) {
      if(target != 0) {
        throw AbiFactError("multiple targets in one ABI fact case");
      }
      target = &record.target;
    } else if(record.kind == ABI_FACT_RECORD_FUNCTION) {
      collect_function_detail(details, record.function);
    }
  }
  if(target == 0) {
    throw AbiFactError("ABI fact case has no target");
  }
  return mangle_target(ctx, *target, details);
}

string mangle_fact_file(const AbiFactFile & file)
{
  string out;
  for(size_t i = 0; i < file.cases.size(); ++i) {
    out += mangle_fact_case(file.cases[i]);
    out += "\n";
  }
  return out;
}

string mangle_fact_files(const vector<string> & input_paths)
{
  string out;
  for(size_t i = 0; i < input_paths.size(); ++i) {
    ifstream in(input_paths[i].c_str());
    if(!in) {
      throw AbiFactError("unable to open input file '" + input_paths[i] + "'");
    }
    ostringstream text;
    text << in.rdbuf();
    out += mangle_fact_file(parse_fact_text(text.str()));
  }
  return out;
}

}  // namespace abi_mangle
