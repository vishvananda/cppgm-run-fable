// Canonical serialization of parsed ABI fact files. The output is the
// space-form spelling of every record; parse_fact_text(serialize_fact_file(f))
// reproduces the same typed facts, which makes round-trip checks possible.

#include "abi/abi_mangle_encode.h"

#include <sstream>

using namespace std;

namespace abi_mangle {

namespace {

string decimal(long long value)
{
  ostringstream out;
  out << value;
  return out.str();
}

string yes_no(bool value)
{
  return value ? "yes" : "no";
}

string serialize_type(const AbiType & type);

// Single-word (colon form) spelling, for list positions that hold one word
// per type. Throws when the type has no single-word spelling.
string serialize_type_word(const AbiType & type)
{
  switch(type.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
  case ABI_TYPE_BUILTIN:
    return type.name;
  case ABI_TYPE_NAMED:
    return "named:" + type.name;
  case ABI_TYPE_POINTER:
    return "ptr:" + serialize_type_word(type.types.front());
  case ABI_TYPE_LVALUE_REFERENCE:
    return "ref:" + serialize_type_word(type.types.front());
  case ABI_TYPE_RVALUE_REFERENCE:
    return "rref:" + serialize_type_word(type.types.front());
  case ABI_TYPE_CV: {
    string out;
    if(type.is_const) {
      out += "const:";
    }
    if(type.is_volatile) {
      out += "volatile:";
    }
    return out + serialize_type_word(type.types.front());
  }
  case ABI_TYPE_ARRAY:
    return "array:" + (type.array_bound.value.empty() ? "-" : type.array_bound.value) + ":" +
           serialize_type_word(type.types.front());
  case ABI_TYPE_MEMBER_POINTER:
    return "memberptr:" + type.types[0].name + ":" + serialize_type_word(type.types[1]);
  case ABI_TYPE_VENDOR_QUALIFIED:
    return "vendor:" + type.name + ":" + serialize_type_word(type.types.front());
  default:
    throw AbiFactError("type fact has no single-word spelling");
  }
}

string serialize_type_word_list(const vector<AbiType> & types)
{
  string out;
  for(size_t i = 0; i < types.size(); ++i) {
    out += " " + serialize_type_word(types[i]);
  }
  return out;
}

string join_refs(const vector<string> & refs)
{
  string out;
  for(size_t i = 0; i < refs.size(); ++i) {
    out += " " + refs[i];
  }
  return out;
}

string serialize_type(const AbiType & type)
{
  switch(type.kind) {
  case ABI_TYPE_TEMPLATE_PARAMETER:
    return string(type.substitutable ? "template-param-subst " : "template-param ") +
           decimal(static_cast<long long>(type.index));
  case ABI_TYPE_NAMED:
    return "name " + type.name;
  case ABI_TYPE_TEMPLATE_SPECIALIZATION:
    return "template " + type.name + join_refs(type.argument_refs);
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
    return "std-template " + type.standard_substitution + " " +
           yes_no(type.standard_substitution_includes_arguments) + " " + type.name +
           join_refs(type.argument_refs);
  case ABI_TYPE_VENDOR_QUALIFIED:
    return "vendor " + type.name + " " + serialize_type(type.types.front());
  case ABI_TYPE_BUILTIN_TRANSFORM:
    return "builtin-transform " + type.name + " " + serialize_type(type.types.front());
  case ABI_TYPE_MEMBER:
    return "member " + serialize_type_word(type.types.front()) + " " + type.name;
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION:
    return "member-template " + serialize_type_word(type.types.front()) + " " + type.name +
           join_refs(type.argument_refs);
  case ABI_TYPE_LOCAL_TYPE:
    return "local-type " + type.context_ref + " " + type.name + " " + type.discriminator;
  case ABI_TYPE_LAMBDA_CLOSURE:
    return "lambda-closure " + type.context_ref + " " + type.discriminator +
           serialize_type_word_list(type.types);
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    return "decltype " + type.expression_ref;
  case ABI_TYPE_CV:
    return string(type.is_const ? "const " : "") + (type.is_volatile ? "volatile " : "") +
           serialize_type(type.types.front());
  case ABI_TYPE_POINTER:
    return "ptr " + serialize_type(type.types.front());
  case ABI_TYPE_LVALUE_REFERENCE:
    return "ref " + serialize_type(type.types.front());
  case ABI_TYPE_RVALUE_REFERENCE:
    return "rref " + serialize_type(type.types.front());
  case ABI_TYPE_PACK_EXPANSION:
    return "pack-expansion " + serialize_type(type.types.front());
  case ABI_TYPE_ARRAY:
    return "array " + (type.array_bound.value.empty() ? "-" : type.array_bound.value) + " " +
           serialize_type(type.types.front());
  case ABI_TYPE_FUNCTION:
    return string(type.variadic ? "function-type-variadic" : "function-type") +
           serialize_type_word_list(type.types);
  default:
    return serialize_type_word(type);
  }
}

string serialize_function_target(const AbiFunctionTarget & target)
{
  switch(target.kind) {
  case ABI_FUNCTION_TARGET_ENCODING:
    return "encoding";
  case ABI_FUNCTION_TARGET_LOCAL:
    return "local " + target.context_ref + " " + target.source_name + " " + target.terminal +
           " " + target.discriminator;
  case ABI_FUNCTION_TARGET_LAMBDA:
    return "lambda " + target.context_ref + " " + target.discriminator + " " + target.terminal +
           serialize_type_word_list(target.signature_parameter_types);
  case ABI_FUNCTION_TARGET_PATH:
    break;
  }
  bool path_form = false;
  string operands;
  for(size_t i = 0; i < target.path_operands.size(); ++i) {
    const AbiFunctionPathOperand & operand = target.path_operands[i];
    if(operand.kind == ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT) {
      path_form = true;
      operands += " " + operand.argument_ref;
    } else if(operand.kind == ABI_FUNCTION_PATH_TYPE) {
      operands += " " + serialize_type_word(operand.type);
    } else {
      throw AbiFactError("unsupported function path operand");
    }
  }
  return (path_form ? "path " : "") + target.qualified_name + operands;
}

string serialize_argument(const AbiTemplateArgument & argument)
{
  switch(argument.kind) {
  case ABI_TEMPLATE_ARGUMENT_TYPE:
    return "type " + serialize_type(argument.type);
  case ABI_TEMPLATE_ARGUMENT_EXPRESSION:
    return "expression " + argument.entity_ref;
  case ABI_TEMPLATE_ARGUMENT_VALUE:
    return "value " + argument.value_type.name + " " + decimal(argument.value);
  case ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE:
    return "dependent-value " + argument.type.name + " " + argument.value_type.name + " " +
           decimal(argument.value);
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY:
    return "template-entity " + argument.name;
  case ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY:
    return "member-template-entity " + argument.owner_type.name + " " + argument.name + " " +
           argument.substitution;
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE:
    return "template-param-template " + decimal(static_cast<long long>(argument.index));
  case ABI_TEMPLATE_ARGUMENT_ENTITY:
    return (argument.address_of ? "entity-address " : "entity ") + argument.entity_ref;
  case ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY: {
    string out = "member-external-address " + argument.symbol + " " + argument.owner_type.name +
                 " " + argument.name + " " + yes_no(argument.member_is_function) + " " +
                 yes_no(argument.member_function_const) + " " + yes_no(argument.member_function_volatile) +
                 " " + yes_no(argument.member_function_lvalue_ref) + " " +
                 yes_no(argument.member_function_rvalue_ref) + " " +
                 yes_no(argument.member_function_variadic);
    for(size_t i = 0; i < argument.parameter_types.size(); ++i) {
      out += " " + serialize_type_word(argument.parameter_types[i]);
    }
    return out;
  }
  case ABI_TEMPLATE_ARGUMENT_PACK:
    return "pack" + join_refs(argument.argument_refs);
  default:
    throw AbiFactError("template argument fact has no serialization");
  }
}

string serialize_expression(const AbiDependentExpression & e)
{
  switch(e.kind) {
  case ABI_EXPRESSION_TEMPLATE_PARAMETER:
    return "template-param " + decimal(static_cast<long long>(e.index));
  case ABI_EXPRESSION_FUNCTION_PARAMETER:
    return "function-param " + decimal(static_cast<long long>(e.index));
  case ABI_EXPRESSION_LITERAL:
    return "literal " + e.text;
  case ABI_EXPRESSION_INTEGRAL_VALUE:
    return "integral-value " + e.type.name + " " + e.text;
  case ABI_EXPRESSION_UNARY:
    return "unary " + e.op + join_refs(e.expression_refs);
  case ABI_EXPRESSION_BINARY:
    return "binary " + e.op + join_refs(e.expression_refs);
  case ABI_EXPRESSION_CONDITIONAL:
    return "conditional" + join_refs(e.expression_refs);
  case ABI_EXPRESSION_PACK_EXPANSION:
    return "pack" + join_refs(e.expression_refs);
  case ABI_EXPRESSION_CALL:
    return "call" + join_refs(e.expression_refs);
  case ABI_EXPRESSION_CONVERSION:
    return "conversion " + serialize_type_word(e.type) + join_refs(e.expression_refs);
  case ABI_EXPRESSION_CAST:
    return "cast " + e.op + " " + serialize_type_word(e.type) + join_refs(e.expression_refs);
  case ABI_EXPRESSION_TEMPLATE_ID:
    return "template-id " + e.text + join_refs(e.argument_refs);
  case ABI_EXPRESSION_TYPE_TRAIT: {
    string out = "type-trait " + e.text;
    for(size_t i = 0; i < e.type_arguments.size(); ++i) {
      out += " " + serialize_type_word(e.type_arguments[i]);
    }
    return out;
  }
  case ABI_EXPRESSION_SIZEOF_TYPE:
    return "sizeof-type " + serialize_type(e.type);
  case ABI_EXPRESSION_MEMBER:
    return "member " + serialize_type_word(e.type) + " " + yes_no(e.close_member_owner) + " " +
           e.text + join_refs(e.argument_refs);
  case ABI_EXPRESSION_OBJECT_MEMBER:
    return "object-member " + e.op + join_refs(e.expression_refs) + " " + e.text +
           join_refs(e.argument_refs);
  case ABI_EXPRESSION_EXTERNAL_ENTITY:
    return "external-entity " + e.text;
  case ABI_EXPRESSION_ENTITY:
    return "entity " + e.entity_ref;
  }
  throw AbiFactError("expression fact has no serialization");
}

string serialize_definition(const AbiDefinitionRecord & definition)
{
  switch(definition.kind) {
  case ABI_DEFINITION_TYPE:
    return "let-type " + definition.id + " " + serialize_type(definition.type);
  case ABI_DEFINITION_TEMPLATE_ARGUMENT:
    return "let-arg " + definition.id + " " + serialize_argument(definition.template_argument);
  case ABI_DEFINITION_EXPRESSION:
    return "let-expr " + definition.id + " " + serialize_expression(definition.expression);
  case ABI_DEFINITION_CONTEXT:
    if(definition.context.kind == ABI_CONTEXT_RAW) {
      return "let-context " + definition.id + " raw " + definition.context.fragment;
    }
    return "let-context " + definition.id + " function " +
           serialize_function_target(definition.context.function);
  case ABI_DEFINITION_ENTITY:
    switch(definition.entity.kind) {
    case ABI_ENTITY_FACT_SYMBOL:
      return "let-entity " + definition.id + " symbol " + definition.entity.qualified_name;
    case ABI_ENTITY_FACT_VARIABLE:
      return "let-entity " + definition.id + " variable " + definition.entity.qualified_name;
    case ABI_ENTITY_FACT_FUNCTION:
      return "let-entity " + definition.id + " function " +
             serialize_function_target(definition.entity.function);
    }
  }
  throw AbiFactError("definition fact has no serialization");
}

string serialize_target(const AbiTargetRecord & target)
{
  switch(target.kind) {
  case ABI_TARGET_FACT_TYPE:
    return "type " + serialize_type(target.type);
  case ABI_TARGET_FACT_FUNCTION:
    return (target.c_linkage ? "c-function " : "function ") + serialize_function_target(target.function);
  case ABI_TARGET_FACT_VARIABLE:
    return "variable " + target.qualified_name;
  case ABI_TARGET_FACT_TYPEINFO:
    return "typeinfo " + serialize_type(target.type);
  case ABI_TARGET_FACT_VTABLE:
    return "vtable " + serialize_type(target.type);
  case ABI_TARGET_FACT_VTT:
    return "vtt " + serialize_type(target.type);
  case ABI_TARGET_FACT_CONSTRUCTION_VTABLE:
    return "construction-vtable " + serialize_type_word(target.type) + " " +
           decimal(static_cast<long long>(target.base_offset)) + " " +
           serialize_type_word(target.base_type);
  case ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER:
    return "tls-wrapper variable " + target.qualified_name;
  case ABI_TARGET_FACT_THUNK:
    if(target.has_result_adjust) {
      return "thunk " + decimal(target.this_adjust) + " " + decimal(target.result_adjust) +
             " function " + serialize_function_target(target.function);
    }
    return "thunk " + decimal(target.this_adjust) + " function " +
           serialize_function_target(target.function);
  case ABI_TARGET_FACT_VIRTUAL_BASE_THUNK:
    return "virtual-base-thunk " + decimal(target.this_adjust) + " " + decimal(target.vcall_offset) +
           " function " + serialize_function_target(target.function);
  }
  throw AbiFactError("target fact has no serialization");
}

string serialize_qualifiers(const vector<AbiFunctionQualifier> & qualifiers)
{
  string out;
  for(size_t i = 0; i < qualifiers.size(); ++i) {
    switch(qualifiers[i]) {
    case ABI_FUNCTION_QUALIFIER_CONST:
      out += " const";
      break;
    case ABI_FUNCTION_QUALIFIER_VOLATILE:
      out += " volatile";
      break;
    case ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE:
      out += " lvalue-ref";
      break;
    case ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE:
      out += " rvalue-ref";
      break;
    }
  }
  return out;
}

string serialize_function_record(const AbiFunctionRecord & record)
{
  switch(record.kind) {
  case ABI_FUNCTION_RECORD_PARAMETER:
    return "param " + serialize_type(record.type);
  case ABI_FUNCTION_RECORD_RESULT:
    return "result " + serialize_type(record.type);
  case ABI_FUNCTION_RECORD_VARIADIC:
    return "variadic";
  case ABI_FUNCTION_RECORD_ABI_TAG:
    return "abi-tag " + record.name;
  case ABI_FUNCTION_RECORD_QUALIFIER:
    return "function-qualifier" + serialize_qualifiers(record.qualifiers);
  case ABI_FUNCTION_RECORD_NAME_STD:
    return "name-std";
  case ABI_FUNCTION_RECORD_NAME_SOURCE:
    return "name-source " + record.name + " " + record.substitution;
  case ABI_FUNCTION_RECORD_NAME_TEMPLATE:
    return "name-template " + record.name + " " + record.substitution + " " +
           record.complete_substitution + " " + record.standard_substitution + " " +
           yes_no(record.standard_substitution_includes_arguments) + join_refs(record.argument_refs);
  case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT:
    return "function-template-arg" + join_refs(record.argument_refs);
  case ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX:
    return "function-template-prefix " + record.substitution;
  case ABI_FUNCTION_RECORD_LOCAL_CONTEXT:
    return "local-context " + record.context_ref + " " + record.source_name + " " + record.discriminator;
  case ABI_FUNCTION_RECORD_LAMBDA_CONTEXT:
    return "lambda-context " + record.context_ref + " " + record.discriminator +
           serialize_type_word_list(record.types);
  case ABI_FUNCTION_RECORD_TERMINAL:
    return "terminal " + record.terminal;
  case ABI_FUNCTION_RECORD_TERMINAL_SOURCE:
    return "terminal-source " + record.source_name;
  case ABI_FUNCTION_RECORD_OPERATOR_TERMINAL:
    if(record.terminal == "literal") {
      return "operator-terminal literal " + record.literal_suffix;
    }
    return "operator-terminal " + record.terminal;
  case ABI_FUNCTION_RECORD_CONVERSION_TERMINAL:
    return "conversion-terminal " + serialize_type(record.type);
  }
  throw AbiFactError("function fact has no serialization");
}

}  // namespace

string serialize_fact_file(const AbiFactFile & file)
{
  string out;
  for(size_t i = 0; i < file.cases.size(); ++i) {
    const AbiFactCase & fact_case = file.cases[i];
    if(i != 0) {
      out += "\n";
    }
    if(!fact_case.label.empty()) {
      out += "case " + fact_case.label + "\n";
    }
    for(size_t j = 0; j < fact_case.records.size(); ++j) {
      const AbiFactRecord & record = fact_case.records[j];
      switch(record.kind) {
      case ABI_FACT_RECORD_DEFINITION:
        out += serialize_definition(record.definition);
        break;
      case ABI_FACT_RECORD_TARGET:
        out += serialize_target(record.target);
        break;
      case ABI_FACT_RECORD_FUNCTION:
        out += serialize_function_record(record.function);
        break;
      }
      out += "\n";
    }
  }
  return out;
}

}  // namespace abi_mangle
