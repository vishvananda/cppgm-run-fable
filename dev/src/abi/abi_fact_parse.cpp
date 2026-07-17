// Parsing of normalized ABI fact files into the typed abi_mangle records.
//
// Fact files are line oriented; each non-blank line is one record. Word
// splitting preserves empty fields ("name-source  -" carries an empty name),
// and type spellings come in a single-word colon form (ptr:const:int) and a
// multi-word space form (template std::allocator Arg).

#include "abi/abi_mangle_encode.h"

#include <cerrno>
#include <cstdlib>

using namespace std;

namespace abi_mangle {

namespace {

vector<string> split_words(const string & line)
{
  vector<string> words(1, string());
  for(size_t i = 0; i < line.size(); ++i) {
    if(line[i] == ' ') {
      words.push_back(string());
    } else {
      words.back() += line[i];
    }
  }
  return words;
}

long long parse_integer(const string & word)
{
  size_t i = word.size() > 1 && word[0] == '-' ? 1 : 0;
  if(i == word.size() || word.size() > 19 + i) {
    throw AbiFactError("malformed integer '" + word + "'");
  }
  for(; i < word.size(); ++i) {
    if(word[i] < '0' || word[i] > '9') {
      throw AbiFactError("malformed integer '" + word + "'");
    }
  }
  errno = 0;
  const long long value = strtoll(word.c_str(), 0, 10);
  if(errno != 0) {
    throw AbiFactError("integer out of range '" + word + "'");
  }
  return value;
}

size_t parse_index(const string & word)
{
  const long long value = parse_integer(word);
  if(value < 0) {
    throw AbiFactError("negative index '" + word + "'");
  }
  return static_cast<size_t>(value);
}

bool is_unsigned_number(const string & word)
{
  if(word.empty()) {
    return false;
  }
  for(size_t i = 0; i < word.size(); ++i) {
    if(word[i] < '0' || word[i] > '9') {
      return false;
    }
  }
  return true;
}

// Local-entity occurrence numbers and lambda numbers: "-" means none.
const string & parse_occurrence_word(const string & word)
{
  if(word != "-" && !is_unsigned_number(word)) {
    throw AbiFactError("malformed occurrence number '" + word + "'");
  }
  return word;
}

bool parse_yes_no(const string & word)
{
  if(word == "yes" || word == "true") {
    return true;
  }
  if(word == "no" || word == "false") {
    return false;
  }
  throw AbiFactError("expected yes/no flag, got '" + word + "'");
}

void require_words(const vector<string> & words, size_t count, const char * what)
{
  if(words.size() < count) {
    throw AbiFactError(string("missing fields in ") + what + " fact");
  }
}

AbiType parse_type_words(const vector<string> & words, size_t begin, size_t end);

AbiType make_reference_type(const string & name)
{
  AbiType type;
  type.kind = ABI_TYPE_NAME_OR_REFERENCE;
  type.name = name;
  return type;
}

AbiType make_builtin_type(const string & name)
{
  AbiType type;
  type.kind = ABI_TYPE_BUILTIN;
  type.name = name;
  return type;
}

vector<string> colon_segments(const string & word)
{
  vector<string> segments(1, string());
  for(size_t i = 0; i < word.size(); ++i) {
    if(word[i] == ':') {
      if(i + 1 < word.size() && word[i + 1] == ':') {
        segments.back() += "::";
        ++i;
      } else {
        segments.push_back(string());
      }
    } else {
      segments.back() += word[i];
    }
  }
  return segments;
}

AbiArrayBound parse_array_bound(const string & word)
{
  AbiArrayBound bound;
  bound.value = word;
  if(word.empty() || word == "-") {
    bound.kind = ABI_ARRAY_BOUND_RAW;
    bound.value.clear();
  } else if(is_unsigned_number(word)) {
    bound.kind = ABI_ARRAY_BOUND_VALUE;
  } else {
    bound.kind = ABI_ARRAY_BOUND_EXPRESSION;
  }
  return bound;
}

AbiType parse_colon_type(const vector<string> & segments, size_t pos)
{
  if(pos >= segments.size()) {
    throw AbiFactError("truncated type spelling");
  }
  const string & head = segments[pos];
  AbiType type;
  if(head == "ptr" || head == "ref" || head == "rref") {
    type.kind = head == "ptr" ? ABI_TYPE_POINTER
              : head == "ref" ? ABI_TYPE_LVALUE_REFERENCE : ABI_TYPE_RVALUE_REFERENCE;
    type.types.push_back(parse_colon_type(segments, pos + 1));
    return type;
  }
  if(head == "const" || head == "volatile") {
    type.kind = ABI_TYPE_CV;
    size_t next = pos;
    while(next < segments.size() && (segments[next] == "const" || segments[next] == "volatile")) {
      type.is_const = type.is_const || segments[next] == "const";
      type.is_volatile = type.is_volatile || segments[next] == "volatile";
      ++next;
    }
    type.types.push_back(parse_colon_type(segments, next));
    return type;
  }
  if(head == "named") {
    if(pos + 2 != segments.size()) {
      throw AbiFactError("named type spelling takes one qualified name");
    }
    type.kind = ABI_TYPE_NAMED;
    type.name = segments[pos + 1];
    return type;
  }
  if(head == "array") {
    if(pos + 2 >= segments.size()) {
      throw AbiFactError("array type spelling needs a bound and element type");
    }
    type.kind = ABI_TYPE_ARRAY;
    type.array_bound = parse_array_bound(segments[pos + 1]);
    type.types.push_back(parse_colon_type(segments, pos + 2));
    return type;
  }
  if(head == "memberptr") {
    if(pos + 2 >= segments.size()) {
      throw AbiFactError("member pointer spelling needs a class and member type");
    }
    type.kind = ABI_TYPE_MEMBER_POINTER;
    AbiType owner;
    owner.kind = ABI_TYPE_NAMED;
    owner.name = segments[pos + 1];
    type.types.push_back(owner);
    type.types.push_back(parse_colon_type(segments, pos + 2));
    return type;
  }
  if(head == "vendor") {
    if(pos + 2 >= segments.size()) {
      throw AbiFactError("vendor type spelling needs a name and inner type");
    }
    type.kind = ABI_TYPE_VENDOR_QUALIFIED;
    type.name = segments[pos + 1];
    type.types.push_back(parse_colon_type(segments, pos + 2));
    return type;
  }
  if(pos + 1 != segments.size()) {
    throw AbiFactError("unknown type spelling head '" + head + "'");
  }
  return make_reference_type(head);
}

AbiType parse_spaced_type(const vector<string> & words, size_t begin, size_t end)
{
  const string & head = words[begin];
  AbiType type;
  if(head == "template-param" || head == "template-param-subst") {
    require_words(words, begin + 2, "template-param");
    type.kind = ABI_TYPE_TEMPLATE_PARAMETER;
    type.index = parse_index(words[begin + 1]);
    type.substitutable = head == "template-param-subst";
    return type;
  }
  if(head == "name" || head == "named") {
    require_words(words, begin + 2, "named type");
    type.kind = ABI_TYPE_NAMED;
    type.name = words[begin + 1];
    return type;
  }
  if(head == "template") {
    require_words(words, begin + 2, "template type");
    type.kind = ABI_TYPE_TEMPLATE_SPECIALIZATION;
    type.name = words[begin + 1];
    type.argument_refs.assign(words.begin() + begin + 2, words.begin() + end);
    return type;
  }
  if(head == "std-template") {
    require_words(words, begin + 4, "std-template type");
    type.kind = ABI_TYPE_STD_TEMPLATE_SPECIALIZATION;
    type.standard_substitution = words[begin + 1];
    type.standard_substitution_includes_arguments = parse_yes_no(words[begin + 2]);
    type.name = words[begin + 3];
    type.argument_refs.assign(words.begin() + begin + 4, words.begin() + end);
    return type;
  }
  if(head == "vendor" || head == "builtin-transform") {
    require_words(words, begin + 3, "qualified type");
    type.kind = head == "vendor" ? ABI_TYPE_VENDOR_QUALIFIED : ABI_TYPE_BUILTIN_TRANSFORM;
    type.name = words[begin + 1];
    type.types.push_back(parse_type_words(words, begin + 2, end));
    return type;
  }
  if(head == "member") {
    require_words(words, begin + 3, "member type");
    type.kind = ABI_TYPE_MEMBER;
    type.types.push_back(parse_type_words(words, begin + 1, begin + 2));
    type.name = words[begin + 2];
    return type;
  }
  if(head == "member-template") {
    require_words(words, begin + 3, "member template type");
    type.kind = ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION;
    type.types.push_back(parse_type_words(words, begin + 1, begin + 2));
    type.name = words[begin + 2];
    type.argument_refs.assign(words.begin() + begin + 3, words.begin() + end);
    return type;
  }
  if(head == "local-type") {
    require_words(words, begin + 4, "local type");
    type.kind = ABI_TYPE_LOCAL_TYPE;
    type.context_ref = words[begin + 1];
    type.name = words[begin + 2];
    type.discriminator = parse_occurrence_word(words[begin + 3]);
    return type;
  }
  if(head == "lambda-closure") {
    require_words(words, begin + 3, "lambda closure type");
    type.kind = ABI_TYPE_LAMBDA_CLOSURE;
    type.context_ref = words[begin + 1];
    type.discriminator = parse_occurrence_word(words[begin + 2]);
    for(size_t i = begin + 3; i < end; ++i) {
      type.types.push_back(parse_type_words(words, i, i + 1));
    }
    return type;
  }
  if(head == "decltype") {
    require_words(words, begin + 2, "decltype type");
    type.kind = ABI_TYPE_DECLTYPE_EXPRESSION;
    type.expression_ref = words[begin + 1];
    return type;
  }
  if(head == "const" || head == "volatile") {
    type.kind = ABI_TYPE_CV;
    type.is_const = head == "const";
    type.is_volatile = head == "volatile";
    type.types.push_back(parse_type_words(words, begin + 1, end));
    return type;
  }
  if(head == "ptr" || head == "ref" || head == "rref" || head == "pack-expansion") {
    type.kind = head == "ptr" ? ABI_TYPE_POINTER
              : head == "ref" ? ABI_TYPE_LVALUE_REFERENCE
              : head == "rref" ? ABI_TYPE_RVALUE_REFERENCE : ABI_TYPE_PACK_EXPANSION;
    type.types.push_back(parse_type_words(words, begin + 1, end));
    return type;
  }
  if(head == "array") {
    require_words(words, begin + 3, "array type");
    type.kind = ABI_TYPE_ARRAY;
    type.array_bound = parse_array_bound(words[begin + 1]);
    type.types.push_back(parse_type_words(words, begin + 2, end));
    return type;
  }
  if(head == "function-type" || head == "function-type-variadic") {
    require_words(words, begin + 2, "function type");
    type.kind = ABI_TYPE_FUNCTION;
    type.variadic = head == "function-type-variadic";
    for(size_t i = begin + 1; i < end; ++i) {
      type.types.push_back(parse_type_words(words, i, i + 1));
    }
    return type;
  }
  throw AbiFactError("unknown type fact head '" + head + "'");
}

AbiType parse_type_words(const vector<string> & words, size_t begin, size_t end)
{
  if(begin >= end || begin >= words.size()) {
    throw AbiFactError("missing type spelling");
  }
  if(end - begin == 1) {
    const vector<string> segments = colon_segments(words[begin]);
    if(segments.size() == 1) {
      return make_reference_type(words[begin]);
    }
    return parse_colon_type(segments, 0);
  }
  return parse_spaced_type(words, begin, end > words.size() ? words.size() : end);
}

AbiFunctionTarget parse_function_target(const vector<string> & words, size_t begin)
{
  AbiFunctionTarget target;
  require_words(words, begin + 1, "function target");
  const string & head = words[begin];
  if(head == "encoding") {
    target.kind = ABI_FUNCTION_TARGET_ENCODING;
    return target;
  }
  if(head == "local") {
    require_words(words, begin + 5, "local function target");
    target.kind = ABI_FUNCTION_TARGET_LOCAL;
    target.context_ref = words[begin + 1];
    target.source_name = words[begin + 2];
    target.terminal = words[begin + 3];
    target.discriminator = parse_occurrence_word(words[begin + 4]);
    return target;
  }
  if(head == "lambda") {
    require_words(words, begin + 4, "lambda function target");
    target.kind = ABI_FUNCTION_TARGET_LAMBDA;
    target.context_ref = words[begin + 1];
    target.discriminator = parse_occurrence_word(words[begin + 2]);
    target.terminal = words[begin + 3];
    for(size_t i = begin + 4; i < words.size(); ++i) {
      target.signature_parameter_types.push_back(parse_type_words(words, i, i + 1));
    }
    return target;
  }
  target.kind = ABI_FUNCTION_TARGET_PATH;
  size_t name_index = begin;
  bool path_form = false;
  if(head == "path") {
    require_words(words, begin + 2, "function path target");
    name_index = begin + 1;
    path_form = true;
  }
  target.qualified_name = words[name_index];
  for(size_t i = name_index + 1; i < words.size(); ++i) {
    AbiFunctionPathOperand operand;
    if(path_form) {
      operand.kind = ABI_FUNCTION_PATH_TEMPLATE_ARGUMENT;
      operand.argument_ref = words[i];
    } else {
      operand.kind = ABI_FUNCTION_PATH_TYPE;
      operand.type = parse_type_words(words, i, i + 1);
    }
    target.path_operands.push_back(operand);
  }
  return target;
}

AbiTemplateArgument parse_template_argument(const vector<string> & words, size_t begin)
{
  AbiTemplateArgument argument;
  require_words(words, begin + 1, "template argument");
  const string & head = words[begin];
  if(head == "type") {
    argument.kind = ABI_TEMPLATE_ARGUMENT_TYPE;
    argument.type = parse_type_words(words, begin + 1, words.size());
    return argument;
  }
  if(head == "expression") {
    require_words(words, begin + 2, "expression argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_EXPRESSION;
    argument.entity_ref = words[begin + 1];
    return argument;
  }
  if(head == "value" || head == "dependent-value") {
    const bool dependent = head == "dependent-value";
    const size_t base = dependent ? begin + 2 : begin + 1;
    require_words(words, base + 2, "value argument");
    argument.kind = dependent ? ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE : ABI_TEMPLATE_ARGUMENT_VALUE;
    if(dependent) {
      argument.type = make_reference_type(words[begin + 1]);
    }
    argument.value_type = make_builtin_type(words[base]);
    argument.has_value_type = true;
    argument.value = parse_integer(words[base + 1]);
    return argument;
  }
  if(head == "template-entity") {
    require_words(words, begin + 2, "template entity argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY;
    argument.name = words[begin + 1];
    return argument;
  }
  if(head == "member-template-entity") {
    require_words(words, begin + 4, "member template entity argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY;
    argument.owner_type = make_reference_type(words[begin + 1]);
    argument.name = words[begin + 2];
    argument.substitution = words[begin + 3];
    return argument;
  }
  if(head == "template-param-template") {
    require_words(words, begin + 2, "template template parameter argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE;
    argument.index = parse_index(words[begin + 1]);
    return argument;
  }
  if(head == "entity-address" || head == "entity") {
    require_words(words, begin + 2, "entity argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_ENTITY;
    argument.entity_ref = words[begin + 1];
    argument.address_of = head == "entity-address";
    return argument;
  }
  if(head == "member-external-address") {
    require_words(words, begin + 10, "member external address argument");
    argument.kind = ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY;
    argument.symbol = words[begin + 1];
    argument.owner_type = make_reference_type(words[begin + 2]);
    argument.name = words[begin + 3];
    argument.member_is_function = parse_yes_no(words[begin + 4]);
    argument.member_function_const = parse_yes_no(words[begin + 5]);
    argument.member_function_volatile = parse_yes_no(words[begin + 6]);
    argument.member_function_lvalue_ref = parse_yes_no(words[begin + 7]);
    argument.member_function_rvalue_ref = parse_yes_no(words[begin + 8]);
    argument.member_function_variadic = parse_yes_no(words[begin + 9]);
    argument.address_of = true;
    for(size_t i = begin + 10; i < words.size(); ++i) {
      argument.parameter_types.push_back(parse_type_words(words, i, i + 1));
    }
    return argument;
  }
  if(head == "pack") {
    argument.kind = ABI_TEMPLATE_ARGUMENT_PACK;
    argument.argument_refs.assign(words.begin() + begin + 1, words.end());
    return argument;
  }
  throw AbiFactError("unknown template argument head '" + head + "'");
}

AbiDependentExpression parse_expression(const vector<string> & words, size_t begin)
{
  AbiDependentExpression e;
  require_words(words, begin + 1, "expression");
  const string & head = words[begin];
  if(head == "template-param" || head == "function-param") {
    require_words(words, begin + 2, "parameter expression");
    e.kind = head == "template-param" ? ABI_EXPRESSION_TEMPLATE_PARAMETER : ABI_EXPRESSION_FUNCTION_PARAMETER;
    e.index = parse_index(words[begin + 1]);
    return e;
  }
  if(head == "literal") {
    require_words(words, begin + 2, "literal expression");
    e.kind = ABI_EXPRESSION_LITERAL;
    e.text = words[begin + 1];
    parse_integer(e.text);
    return e;
  }
  if(head == "integral-value") {
    require_words(words, begin + 3, "integral value expression");
    e.kind = ABI_EXPRESSION_INTEGRAL_VALUE;
    e.type = make_builtin_type(words[begin + 1]);
    e.text = words[begin + 2];
    parse_integer(e.text);
    return e;
  }
  if(head == "unary" || head == "binary") {
    const size_t operands = head == "unary" ? 1 : 2;
    require_words(words, begin + 2 + operands, "operator expression");
    e.kind = head == "unary" ? ABI_EXPRESSION_UNARY : ABI_EXPRESSION_BINARY;
    e.op = words[begin + 1];
    e.expression_refs.assign(words.begin() + begin + 2, words.begin() + begin + 2 + operands);
    return e;
  }
  if(head == "conditional") {
    require_words(words, begin + 4, "conditional expression");
    e.kind = ABI_EXPRESSION_CONDITIONAL;
    e.expression_refs.assign(words.begin() + begin + 1, words.begin() + begin + 4);
    return e;
  }
  if(head == "pack") {
    require_words(words, begin + 2, "pack expression");
    e.kind = ABI_EXPRESSION_PACK_EXPANSION;
    e.expression_refs.push_back(words[begin + 1]);
    return e;
  }
  if(head == "call") {
    require_words(words, begin + 2, "call expression");
    e.kind = ABI_EXPRESSION_CALL;
    e.expression_refs.assign(words.begin() + begin + 1, words.end());
    return e;
  }
  if(head == "conversion" || head == "cast") {
    const bool is_cast = head == "cast";
    const size_t base = is_cast ? begin + 2 : begin + 1;
    require_words(words, base + 2, "conversion expression");
    e.kind = is_cast ? ABI_EXPRESSION_CAST : ABI_EXPRESSION_CONVERSION;
    if(is_cast) {
      e.op = words[begin + 1];
    }
    e.type = parse_type_words(words, base, base + 1);
    e.expression_refs.push_back(words[base + 1]);
    return e;
  }
  if(head == "template-id") {
    require_words(words, begin + 2, "template-id expression");
    e.kind = ABI_EXPRESSION_TEMPLATE_ID;
    e.text = words[begin + 1];
    e.argument_refs.assign(words.begin() + begin + 2, words.end());
    return e;
  }
  if(head == "type-trait") {
    require_words(words, begin + 2, "type trait expression");
    e.kind = ABI_EXPRESSION_TYPE_TRAIT;
    e.text = words[begin + 1];
    for(size_t i = begin + 2; i < words.size(); ++i) {
      e.type_arguments.push_back(parse_type_words(words, i, i + 1));
    }
    return e;
  }
  if(head == "sizeof-type") {
    e.kind = ABI_EXPRESSION_SIZEOF_TYPE;
    e.type = parse_type_words(words, begin + 1, words.size());
    return e;
  }
  if(head == "member") {
    require_words(words, begin + 4, "member expression");
    e.kind = ABI_EXPRESSION_MEMBER;
    e.type = parse_type_words(words, begin + 1, begin + 2);
    e.close_member_owner = parse_yes_no(words[begin + 2]);
    e.text = words[begin + 3];
    e.argument_refs.assign(words.begin() + begin + 4, words.end());
    return e;
  }
  if(head == "object-member") {
    require_words(words, begin + 4, "object member expression");
    e.kind = ABI_EXPRESSION_OBJECT_MEMBER;
    e.op = words[begin + 1];
    e.expression_refs.push_back(words[begin + 2]);
    e.text = words[begin + 3];
    e.argument_refs.assign(words.begin() + begin + 4, words.end());
    return e;
  }
  if(head == "external-entity") {
    require_words(words, begin + 2, "external entity expression");
    e.kind = ABI_EXPRESSION_EXTERNAL_ENTITY;
    e.text = words[begin + 1];
    return e;
  }
  if(head == "entity") {
    require_words(words, begin + 2, "entity expression");
    e.kind = ABI_EXPRESSION_ENTITY;
    e.entity_ref = words[begin + 1];
    return e;
  }
  throw AbiFactError("unknown expression head '" + head + "'");
}

AbiDefinitionRecord parse_definition(const vector<string> & words)
{
  AbiDefinitionRecord definition;
  require_words(words, 3, "definition");
  definition.id = words[1];
  const string & keyword = words[0];
  if(keyword == "let-type") {
    definition.kind = ABI_DEFINITION_TYPE;
    definition.type = parse_type_words(words, 2, words.size());
    return definition;
  }
  if(keyword == "let-arg") {
    definition.kind = ABI_DEFINITION_TEMPLATE_ARGUMENT;
    definition.template_argument = parse_template_argument(words, 2);
    return definition;
  }
  if(keyword == "let-expr") {
    definition.kind = ABI_DEFINITION_EXPRESSION;
    definition.expression = parse_expression(words, 2);
    return definition;
  }
  if(keyword == "let-context") {
    definition.kind = ABI_DEFINITION_CONTEXT;
    require_words(words, 4, "context definition");
    if(words[2] == "raw") {
      definition.context.kind = ABI_CONTEXT_RAW;
      definition.context.fragment = words[3];
      return definition;
    }
    if(words[2] != "function") {
      throw AbiFactError("unknown context definition kind '" + words[2] + "'");
    }
    definition.context.kind = ABI_CONTEXT_FUNCTION;
    definition.context.function = parse_function_target(words, 3);
    return definition;
  }
  if(keyword == "let-entity") {
    definition.kind = ABI_DEFINITION_ENTITY;
    if(words[2] == "symbol") {
      require_words(words, 4, "symbol entity definition");
      definition.entity.kind = ABI_ENTITY_FACT_SYMBOL;
      definition.entity.qualified_name = words[3];
      return definition;
    }
    if(words[2] == "variable") {
      require_words(words, 4, "variable entity definition");
      definition.entity.kind = ABI_ENTITY_FACT_VARIABLE;
      definition.entity.qualified_name = words[3];
      return definition;
    }
    if(words[2] != "function") {
      throw AbiFactError("unknown entity definition kind '" + words[2] + "'");
    }
    definition.entity.kind = ABI_ENTITY_FACT_FUNCTION;
    definition.entity.function = parse_function_target(words, 3);
    return definition;
  }
  throw AbiFactError("unknown definition keyword '" + keyword + "'");
}

size_t parse_thunk_offsets(const vector<string> & words, vector<long long> & offsets)
{
  size_t i = 1;
  while(i < words.size() && words[i] != "function") {
    offsets.push_back(parse_integer(words[i]));
    ++i;
  }
  if(i == words.size() || offsets.empty() || offsets.size() > 2) {
    throw AbiFactError("malformed thunk target");
  }
  return i + 1;
}

AbiTargetRecord parse_target(const vector<string> & words)
{
  AbiTargetRecord target;
  const string & keyword = words[0];
  if(keyword == "type" || keyword == "typeinfo" || keyword == "vtable" || keyword == "vtt") {
    target.kind = keyword == "type" ? ABI_TARGET_FACT_TYPE
                : keyword == "typeinfo" ? ABI_TARGET_FACT_TYPEINFO
                : keyword == "vtable" ? ABI_TARGET_FACT_VTABLE : ABI_TARGET_FACT_VTT;
    target.type = parse_type_words(words, 1, words.size());
    return target;
  }
  if(keyword == "function" || keyword == "c-function") {
    target.kind = ABI_TARGET_FACT_FUNCTION;
    target.c_linkage = keyword == "c-function";
    target.function = parse_function_target(words, 1);
    return target;
  }
  if(keyword == "variable") {
    require_words(words, 2, "variable target");
    target.kind = ABI_TARGET_FACT_VARIABLE;
    target.qualified_name = words[1];
    return target;
  }
  if(keyword == "construction-vtable") {
    require_words(words, 4, "construction vtable target");
    target.kind = ABI_TARGET_FACT_CONSTRUCTION_VTABLE;
    target.type = parse_type_words(words, 1, 2);
    target.base_offset = parse_index(words[2]);
    target.base_type = parse_type_words(words, 3, 4);
    return target;
  }
  if(keyword == "tls-wrapper") {
    require_words(words, 3, "tls wrapper target");
    if(words[1] != "variable") {
      throw AbiFactError("tls-wrapper target must name a variable");
    }
    target.kind = ABI_TARGET_FACT_THREAD_LOCAL_WRAPPER;
    target.qualified_name = words[2];
    return target;
  }
  if(keyword == "thunk" || keyword == "virtual-base-thunk") {
    vector<long long> offsets;
    const size_t function_begin = parse_thunk_offsets(words, offsets);
    target.function = parse_function_target(words, function_begin);
    if(keyword == "thunk") {
      target.kind = ABI_TARGET_FACT_THUNK;
      target.this_adjust = offsets[0];
      if(offsets.size() == 2) {
        target.has_result_adjust = true;
        target.result_adjust = offsets[1];
      }
    } else {
      target.kind = ABI_TARGET_FACT_VIRTUAL_BASE_THUNK;
      target.this_adjust = offsets.size() == 2 ? offsets[0] : 0;
      target.vcall_offset = offsets.size() == 2 ? offsets[1] : offsets[0];
    }
    return target;
  }
  throw AbiFactError("unknown target keyword '" + keyword + "'");
}

AbiFunctionQualifier parse_function_qualifier(const string & word)
{
  if(word == "const") {
    return ABI_FUNCTION_QUALIFIER_CONST;
  }
  if(word == "volatile") {
    return ABI_FUNCTION_QUALIFIER_VOLATILE;
  }
  if(word == "lvalue-ref" || word == "&") {
    return ABI_FUNCTION_QUALIFIER_LVALUE_REFERENCE;
  }
  if(word == "rvalue-ref" || word == "&&") {
    return ABI_FUNCTION_QUALIFIER_RVALUE_REFERENCE;
  }
  throw AbiFactError("unknown function qualifier '" + word + "'");
}

AbiFunctionRecord parse_function_record(const vector<string> & words)
{
  AbiFunctionRecord record;
  const string & keyword = words[0];
  if(keyword == "param" || keyword == "result") {
    record.kind = keyword == "param" ? ABI_FUNCTION_RECORD_PARAMETER : ABI_FUNCTION_RECORD_RESULT;
    record.type = parse_type_words(words, 1, words.size());
    return record;
  }
  if(keyword == "variadic") {
    record.kind = ABI_FUNCTION_RECORD_VARIADIC;
    return record;
  }
  if(keyword == "abi-tag") {
    require_words(words, 2, "abi tag");
    record.kind = ABI_FUNCTION_RECORD_ABI_TAG;
    record.name = words[1];
    return record;
  }
  if(keyword == "function-qualifier") {
    require_words(words, 2, "function qualifier");
    record.kind = ABI_FUNCTION_RECORD_QUALIFIER;
    for(size_t i = 1; i < words.size(); ++i) {
      record.qualifiers.push_back(parse_function_qualifier(words[i]));
    }
    return record;
  }
  if(keyword == "name-std") {
    record.kind = ABI_FUNCTION_RECORD_NAME_STD;
    return record;
  }
  if(keyword == "name-source") {
    require_words(words, 3, "name-source");
    record.kind = ABI_FUNCTION_RECORD_NAME_SOURCE;
    record.name = words[1];
    record.substitution = words[2];
    return record;
  }
  if(keyword == "name-template") {
    require_words(words, 6, "name-template");
    record.kind = ABI_FUNCTION_RECORD_NAME_TEMPLATE;
    record.name = words[1];
    record.substitution = words[2];
    record.complete_substitution = words[3];
    record.standard_substitution = words[4];
    record.standard_substitution_includes_arguments = parse_yes_no(words[5]);
    record.argument_refs.assign(words.begin() + 6, words.end());
    return record;
  }
  if(keyword == "function-template-arg") {
    require_words(words, 2, "function template argument");
    record.kind = ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_ARGUMENT;
    record.argument_refs.assign(words.begin() + 1, words.end());
    return record;
  }
  if(keyword == "function-template-prefix") {
    require_words(words, 2, "function template prefix");
    record.kind = ABI_FUNCTION_RECORD_FUNCTION_TEMPLATE_PREFIX;
    record.substitution = words[1];
    return record;
  }
  if(keyword == "local-context") {
    require_words(words, 4, "local context");
    record.kind = ABI_FUNCTION_RECORD_LOCAL_CONTEXT;
    record.context_ref = words[1];
    record.source_name = words[2];
    record.discriminator = parse_occurrence_word(words[3]);
    return record;
  }
  if(keyword == "lambda-context") {
    require_words(words, 3, "lambda context");
    record.kind = ABI_FUNCTION_RECORD_LAMBDA_CONTEXT;
    record.context_ref = words[1];
    record.discriminator = parse_occurrence_word(words[2]);
    for(size_t i = 3; i < words.size(); ++i) {
      record.types.push_back(parse_type_words(words, i, i + 1));
    }
    return record;
  }
  if(keyword == "terminal") {
    require_words(words, 2, "terminal");
    record.kind = ABI_FUNCTION_RECORD_TERMINAL;
    record.terminal = words[1];
    return record;
  }
  if(keyword == "terminal-source") {
    require_words(words, 2, "terminal source");
    record.kind = ABI_FUNCTION_RECORD_TERMINAL_SOURCE;
    record.source_name = words[1];
    return record;
  }
  if(keyword == "operator-terminal") {
    require_words(words, 2, "operator terminal");
    record.kind = ABI_FUNCTION_RECORD_OPERATOR_TERMINAL;
    record.terminal = words[1];
    if(record.terminal == "literal") {
      require_words(words, 3, "literal operator terminal");
      record.literal_suffix = words[2];
    }
    return record;
  }
  if(keyword == "conversion-terminal") {
    record.kind = ABI_FUNCTION_RECORD_CONVERSION_TERMINAL;
    record.type = parse_type_words(words, 1, words.size());
    return record;
  }
  throw AbiFactError("unknown fact keyword '" + keyword + "'");
}

}  // namespace

AbiFactRecord parse_fact_record_words(const vector<string> & words)
{
  if(words.empty() || words[0].empty()) {
    throw AbiFactError("empty ABI fact record");
  }
  AbiFactRecord record;
  const string & keyword = words[0];
  if(keyword.compare(0, 4, "let-") == 0) {
    record.kind = ABI_FACT_RECORD_DEFINITION;
    record.definition = parse_definition(words);
    return record;
  }
  if(keyword == "type" || keyword == "function" || keyword == "c-function" ||
     keyword == "variable" || keyword == "typeinfo" || keyword == "vtable" ||
     keyword == "vtt" || keyword == "construction-vtable" || keyword == "tls-wrapper" ||
     keyword == "thunk" || keyword == "virtual-base-thunk") {
    record.kind = ABI_FACT_RECORD_TARGET;
    record.target = parse_target(words);
    return record;
  }
  record.kind = ABI_FACT_RECORD_FUNCTION;
  record.function = parse_function_record(words);
  return record;
}

AbiFactFile parse_fact_text(const string & text)
{
  AbiFactFile file;
  size_t pos = 0;
  while(pos <= text.size()) {
    const size_t eol = text.find('\n', pos);
    string line = text.substr(pos, eol == string::npos ? string::npos : eol - pos);
    pos = eol == string::npos ? text.size() + 1 : eol + 1;
    if(!line.empty() && line[line.size() - 1] == '\r') {
      line.erase(line.size() - 1);
    }
    if(line.find_first_not_of(' ') == string::npos) {
      continue;
    }
    const vector<string> words = split_words(line);
    if(words[0] == "case") {
      require_words(words, 2, "case");
      file.cases.push_back(AbiFactCase());
      file.cases.back().label = words[1];
      continue;
    }
    if(file.cases.empty()) {
      file.cases.push_back(AbiFactCase());
    }
    file.cases.back().records.push_back(parse_fact_record_words(words));
  }
  return file;
}

}  // namespace abi_mangle
