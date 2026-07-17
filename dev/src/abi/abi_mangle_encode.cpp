// Itanium encoding of ABI fact types, template arguments, and expressions.
//
// Substitution is structural: every substitutable component is identified by
// a canonical key (or an explicit key carried by the fact file), and one
// SubstitutionTable per mangled name maps keys to seq-ids. Components whose
// facts say they are not substitutable (plain template-param references,
// builtins, std abbreviations) never touch the table.

#include "abi/abi_mangle_encode.h"

#include <cstdlib>
#include <sstream>

using namespace std;

namespace abi_mangle {

namespace {

string seq_id_code(size_t index)
{
  if(index == 0) {
    return "S_";
  }
  static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  string body;
  size_t value = index - 1;
  do {
    body.insert(body.begin(), digits[value % 36]);
    value /= 36;
  } while(value != 0);
  return "S" + body + "_";
}

string decimal(long long value)
{
  ostringstream out;
  out << value;
  return out.str();
}

string template_parameter_spelling(size_t index)
{
  if(index == 0) {
    return "T_";
  }
  return "T" + decimal(static_cast<long long>(index - 1)) + "_";
}

struct CvFlags
{
  bool is_const = false;
  bool is_volatile = false;
};

// Follows references and collapses stacked cv layers into one qualifier set,
// as the Itanium grammar treats combined cv-qualifiers as a single component.
const AbiType & strip_cv(const EncodeContext & ctx, const AbiType & type, CvFlags & flags)
{
  const AbiType * current = &resolve_type(ctx, type);
  int depth = 0;
  while(current->kind == ABI_TYPE_CV) {
    flags.is_const = flags.is_const || current->is_const;
    flags.is_volatile = flags.is_volatile || current->is_volatile;
    if(current->types.empty()) {
      throw AbiFactError("cv type fact is missing its inner type");
    }
    current = &resolve_type(ctx, current->types.front());
    if(++depth > 64) {
      throw AbiFactError("cv type facts nest too deeply");
    }
  }
  return *current;
}

}  // namespace

bool SubstitutionTable::try_emit(const string & key, string & out) const
{
  if(key.empty()) {
    return false;
  }
  map<string, size_t>::const_iterator it = index_by_key_.find(key);
  if(it == index_by_key_.end()) {
    return false;
  }
  out += seq_id_code(it->second);
  return true;
}

bool SubstitutionTable::contains(const string & key) const
{
  return !key.empty() && index_by_key_.count(key) != 0;
}

void SubstitutionTable::enter(const string & key)
{
  if(key.empty() || index_by_key_.count(key) != 0) {
    return;
  }
  const size_t next = index_by_key_.size();
  index_by_key_[key] = next;
}

const char * builtin_type_code(const string & name)
{
  static const struct { const char * name; const char * code; } table[] = {
    {"void", "v"},      {"bool", "b"},        {"char", "c"},
    {"schar", "a"},     {"signed-char", "a"}, {"uchar", "h"},
    {"unsigned-char", "h"}, {"short", "s"},   {"ushort", "t"},
    {"int", "i"},       {"uint", "j"},        {"unsigned", "j"},
    {"long", "l"},      {"ulong", "m"},       {"longlong", "x"},
    {"ulonglong", "y"}, {"int128", "n"},      {"uint128", "o"},
    {"float", "f"},     {"double", "d"},      {"longdouble", "e"},
    {"float128", "g"},  {"wchar", "w"},       {"char8", "Du"},
    {"char16", "Ds"},   {"char32", "Di"},     {"auto", "Da"},
    {"decltype-auto", "Dc"}, {"nullptr", "Dn"}, {"varargs", "z"},
    {"ellipsis", "z"},
  };
  for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
    if(name == table[i].name) {
      return table[i].code;
    }
  }
  return 0;
}

string source_name(const string & name)
{
  if(name.empty()) {
    throw AbiFactError("empty source name in ABI fact");
  }
  return decimal(static_cast<long long>(name.size())) + name;
}

string integer_literal(const string & type_name, const string & value)
{
  const char * code = builtin_type_code(type_name);
  if(code == 0) {
    throw AbiFactError("unknown literal type '" + type_name + "'");
  }
  string digits = value;
  if(!digits.empty() && digits[0] == '-') {
    digits = "n" + digits.substr(1);
  }
  return "L" + string(code) + digits + "E";
}

const AbiType & resolve_type(const EncodeContext & ctx, const AbiType & type)
{
  const AbiType * current = &type;
  int depth = 0;
  while(current->kind == ABI_TYPE_NAME_OR_REFERENCE) {
    map<string, const AbiType *>::const_iterator it = ctx.env.types.find(current->name);
    if(it == ctx.env.types.end()) {
      if(builtin_type_code(current->name) != 0) {
        return *current;
      }
      throw AbiFactError("unknown type reference '" + current->name + "'");
    }
    current = it->second;
    if(++depth > 64) {
      throw AbiFactError("type reference cycle at '" + type.name + "'");
    }
  }
  return *current;
}

namespace {

string argument_list_key(const EncodeContext & ctx, const vector<string> & argument_refs)
{
  string key = "<";
  for(size_t i = 0; i < argument_refs.size(); ++i) {
    if(i != 0) {
      key += ",";
    }
    key += argument_key(ctx, ctx.env.argument(argument_refs[i]));
  }
  return key + ">";
}

string function_type_key(const EncodeContext & ctx, const AbiType & type)
{
  string key = "F(";
  for(size_t i = 0; i < type.types.size(); ++i) {
    if(i != 0) {
      key += ",";
    }
    key += type_key(ctx, type.types[i]);
  }
  if(type.variadic) {
    key += ",...";
  }
  return key + ")";
}

string cv_key(const EncodeContext & ctx, const AbiType & type)
{
  CvFlags flags;
  const AbiType & inner = strip_cv(ctx, type, flags);
  string quals;
  if(flags.is_volatile) {
    quals += "V";
  }
  if(flags.is_const) {
    quals += "K";
  }
  if(quals.empty()) {
    return type_key(ctx, inner);
  }
  return "q" + quals + ":" + type_key(ctx, inner);
}

}  // namespace

string type_key(const EncodeContext & ctx, const AbiType & raw)
{
  const AbiType & t = resolve_type(ctx, raw);
  switch(t.kind) {
  case ABI_TYPE_NAME_OR_REFERENCE:
  case ABI_TYPE_BUILTIN:
    return t.name;
  case ABI_TYPE_NAMED:
    return t.name;
  case ABI_TYPE_TEMPLATE_PARAMETER:
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION:
    return "tp#" + decimal(static_cast<long long>(t.index));
  case ABI_TYPE_POINTER:
    return "P:" + type_key(ctx, t.types.front());
  case ABI_TYPE_LVALUE_REFERENCE:
    return "R:" + type_key(ctx, t.types.front());
  case ABI_TYPE_RVALUE_REFERENCE:
    return "O:" + type_key(ctx, t.types.front());
  case ABI_TYPE_CV:
    return cv_key(ctx, t);
  case ABI_TYPE_PACK_EXPANSION:
    return "Dp:" + type_key(ctx, t.types.front());
  case ABI_TYPE_VENDOR_QUALIFIED:
    return "U:" + t.name + ":" + type_key(ctx, t.types.front());
  case ABI_TYPE_ARRAY:
    return "A:" + t.array_bound.value + ":" + type_key(ctx, t.types.front());
  case ABI_TYPE_BUILTIN_TRANSFORM:
    return "u:" + t.name + ":" + type_key(ctx, t.types.front());
  case ABI_TYPE_FUNCTION:
    return function_type_key(ctx, t);
  case ABI_TYPE_MEMBER_POINTER:
    return "M:" + type_key(ctx, t.types[0]) + ":" + type_key(ctx, t.types[1]);
  case ABI_TYPE_TEMPLATE_SPECIALIZATION:
  case ABI_TYPE_STD_TEMPLATE_SPECIALIZATION:
    return t.name + argument_list_key(ctx, t.argument_refs);
  case ABI_TYPE_MEMBER:
    return type_key(ctx, t.types.front()) + "::" + t.name;
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION:
    return type_key(ctx, t.types.front()) + "::" + t.name + argument_list_key(ctx, t.argument_refs);
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    return "DT:" + expression_key(ctx, ctx.env.expression(t.expression_ref));
  case ABI_TYPE_LAMBDA_CLOSURE: {
    string sig;
    for(size_t i = 0; i < t.types.size(); ++i) {
      sig += (i != 0 ? "," : "") + type_key(ctx, t.types[i]);
    }
    return "Zl:" + t.context_ref + "#" + t.discriminator + "(" + sig + ")";
  }
  case ABI_TYPE_LOCAL_TYPE:
    return "Zt:" + t.context_ref + "::" + t.name + "#" + t.discriminator;
  }
  throw AbiFactError("unhandled type fact kind in type_key");
}

string argument_key(const EncodeContext & ctx, const AbiTemplateArgument & argument)
{
  switch(argument.kind) {
  case ABI_TEMPLATE_ARGUMENT_TYPE:
    return type_key(ctx, argument.type);
  case ABI_TEMPLATE_ARGUMENT_VALUE:
  case ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE:
    return "L:" + argument.value_type.name + ":" + decimal(argument.value);
  case ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE:
    return "Tn:" + type_key(ctx, argument.type) + ":" + argument.value_type.name + ":" + decimal(argument.value);
  case ABI_TEMPLATE_ARGUMENT_EXPRESSION:
    return "X:" + expression_key(ctx, ctx.env.expression(argument.entity_ref));
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY:
    return argument.name;
  case ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY:
    return argument.substitution;
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE:
    return "tp#" + decimal(static_cast<long long>(argument.index));
  case ABI_TEMPLATE_ARGUMENT_ENTITY:
    return "ent:" + argument.entity_ref;
  case ABI_TEMPLATE_ARGUMENT_EXTERNAL_ENTITY:
  case ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY:
    return "sym:" + argument.symbol;
  case ABI_TEMPLATE_ARGUMENT_PACK: {
    string key = "J";
    for(size_t i = 0; i < argument.argument_refs.size(); ++i) {
      key += ":" + argument_key(ctx, ctx.env.argument(argument.argument_refs[i]));
    }
    return key;
  }
  }
  throw AbiFactError("unhandled template argument kind in argument_key");
}

string expression_key(const EncodeContext & ctx, const AbiDependentExpression & e)
{
  string key = "e" + decimal(static_cast<long long>(e.kind)) + ":" + e.op + ":" + e.text;
  key += ":" + decimal(static_cast<long long>(e.index));
  for(size_t i = 0; i < e.expression_refs.size(); ++i) {
    key += "(" + expression_key(ctx, ctx.env.expression(e.expression_refs[i])) + ")";
  }
  for(size_t i = 0; i < e.argument_refs.size(); ++i) {
    key += "[" + argument_key(ctx, ctx.env.argument(e.argument_refs[i])) + "]";
  }
  for(size_t i = 0; i < e.type_arguments.size(); ++i) {
    key += "{" + type_key(ctx, e.type_arguments[i]) + "}";
  }
  if(e.kind == ABI_EXPRESSION_MEMBER || e.kind == ABI_EXPRESSION_SIZEOF_TYPE ||
     e.kind == ABI_EXPRESSION_CONVERSION || e.kind == ABI_EXPRESSION_CAST) {
    key += "<" + type_key(ctx, e.type) + ">";
  }
  if(!e.entity_ref.empty()) {
    key += "@" + e.entity_ref;
  }
  return key;
}

void encode_argument_list(EncodeContext & ctx, const vector<string> & argument_refs, string & out)
{
  out += "I";
  for(size_t i = 0; i < argument_refs.size(); ++i) {
    encode_template_argument(ctx, ctx.env.argument(argument_refs[i]), out);
  }
  out += "E";
}

vector<NameLink> qualified_name_links(const string & qualified_name, bool keyed)
{
  string name = qualified_name;
  if(name.compare(0, 2, "::") == 0) {
    name = name.substr(2);
  }
  vector<string> parts;
  string::size_type pos = 0;
  while(true) {
    const string::size_type sep = name.find("::", pos);
    if(sep == string::npos) {
      parts.push_back(name.substr(pos));
      break;
    }
    parts.push_back(name.substr(pos, sep - pos));
    pos = sep + 2;
  }
  vector<NameLink> links;
  string cumulative;
  size_t first = 0;
  if(parts.size() > 1 && parts[0] == "std") {
    NameLink std_link;
    std_link.spelled = "St";
    std_link.std_abbrev = true;
    links.push_back(std_link);
    cumulative = "std";
    first = 1;
  }
  for(size_t i = first; i < parts.size(); ++i) {
    if(parts[i].empty()) {
      throw AbiFactError("malformed qualified name '" + qualified_name + "'");
    }
    cumulative += cumulative.empty() ? parts[i] : "::" + parts[i];
    NameLink link;
    link.spelled = source_name(parts[i]);
    link.key = keyed ? cumulative : string();
    links.push_back(link);
  }
  return links;
}

namespace {

struct LinkCut
{
  size_t index = 0;
  bool after_args = false;
  bool found = false;
};

LinkCut find_link_cut(const EncodeContext & ctx, const vector<NameLink> & links)
{
  LinkCut cut;
  for(size_t i = links.size(); i > 0; --i) {
    const NameLink & link = links[i - 1];
    if(link.has_args && ctx.subs.contains(link.key_with_args)) {
      cut.index = i - 1;
      cut.after_args = true;
      cut.found = true;
      return cut;
    }
    if(ctx.subs.contains(link.key)) {
      cut.index = i - 1;
      cut.found = true;
      return cut;
    }
  }
  return cut;
}

void emit_one_link(EncodeContext & ctx, const NameLink & link, string & out)
{
  out += link.spelled;
  if(link.type_piece != 0) {
    encode_type(ctx, *link.type_piece, out);
  }
  out += link.suffix;
  ctx.subs.enter(link.key);
  if(link.has_args) {
    encode_argument_list(ctx, link.argument_refs, out);
    ctx.subs.enter(link.key_with_args);
  }
}

void emit_link_sequence(EncodeContext & ctx, const vector<NameLink> & links, string & out)
{
  const LinkCut cut = find_link_cut(ctx, links);
  size_t start = 0;
  if(cut.found) {
    const NameLink & link = links[cut.index];
    ctx.subs.try_emit(cut.after_args ? link.key_with_args : link.key, out);
    if(!cut.after_args && link.has_args) {
      encode_argument_list(ctx, link.argument_refs, out);
      ctx.subs.enter(link.key_with_args);
    }
    start = cut.index + 1;
  }
  for(size_t i = start; i < links.size(); ++i) {
    emit_one_link(ctx, links[i], out);
  }
}

}  // namespace

void emit_name_links(EncodeContext & ctx, const vector<NameLink> & links, const string & qualifiers, string & out)
{
  if(links.empty()) {
    throw AbiFactError("ABI name has no components");
  }
  const bool std_merged = links.size() == 2 && links[0].std_abbrev;
  const bool nested = !qualifiers.empty() || (!std_merged && links.size() > 1);
  if(nested) {
    out += "N";
    out += qualifiers;
  }
  emit_link_sequence(ctx, links, out);
  if(nested) {
    out += "E";
  }
}

namespace {

// Prefix links for a member owner: named and template owners expand into
// their qualified components so prefix-level substitution applies; dependent
// owners (template parameters, transforms, decltype) stay one type piece.
vector<NameLink> owner_prefix_links(EncodeContext & ctx, const AbiType & raw)
{
  const AbiType & owner = resolve_type(ctx, raw);
  if(owner.kind == ABI_TYPE_NAMED) {
    return qualified_name_links(owner.name, true);
  }
  if(owner.kind == ABI_TYPE_TEMPLATE_SPECIALIZATION || owner.kind == ABI_TYPE_STD_TEMPLATE_SPECIALIZATION) {
    vector<NameLink> links = qualified_name_links(owner.name, true);
    links.back().has_args = true;
    links.back().argument_refs = owner.argument_refs;
    links.back().key_with_args = type_key(ctx, owner);
    return links;
  }
  if(owner.kind == ABI_TYPE_MEMBER || owner.kind == ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION) {
    vector<NameLink> links = owner_prefix_links(ctx, owner.types.front());
    NameLink link;
    link.spelled = source_name(owner.name);
    link.key = type_key(ctx, owner);
    if(owner.kind == ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION) {
      link.key = type_key(ctx, owner.types.front()) + "::" + owner.name;
      link.has_args = true;
      link.argument_refs = owner.argument_refs;
      link.key_with_args = type_key(ctx, owner);
    }
    links.push_back(link);
    return links;
  }
  vector<NameLink> links;
  NameLink link;
  link.type_piece = &raw;
  links.push_back(link);
  return links;
}

void encode_member_type(EncodeContext & ctx, const AbiType & t, string & out)
{
  vector<NameLink> links = owner_prefix_links(ctx, t.types.front());
  NameLink link;
  link.spelled = source_name(t.name);
  link.key = type_key(ctx, t.types.front()) + "::" + t.name;
  if(t.kind == ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION) {
    link.has_args = true;
    link.argument_refs = t.argument_refs;
    link.key_with_args = type_key(ctx, t);
  }
  links.push_back(link);
  out += "N";
  emit_link_sequence(ctx, links, out);
  out += "E";
}

void encode_std_template(EncodeContext & ctx, const AbiType & t, string & out)
{
  const string key = type_key(ctx, t);
  if(t.standard_substitution_includes_arguments) {
    out += t.standard_substitution;
    return;
  }
  if(ctx.subs.try_emit(key, out)) {
    return;
  }
  out += t.standard_substitution;
  encode_argument_list(ctx, t.argument_refs, out);
  ctx.subs.enter(key);
}

void encode_function_type(EncodeContext & ctx, const AbiType & t, string & out)
{
  if(t.types.empty()) {
    throw AbiFactError("function type fact needs a return type");
  }
  out += "F";
  encode_type(ctx, t.types.front(), out);
  if(t.types.size() == 1) {
    out += "v";
  }
  for(size_t i = 1; i < t.types.size(); ++i) {
    encode_type(ctx, t.types[i], out);
  }
  if(t.variadic) {
    out += "z";
  }
  out += "E";
}

void encode_array_type(EncodeContext & ctx, const AbiType & t, string & out)
{
  out += "A";
  switch(t.array_bound.kind) {
  case ABI_ARRAY_BOUND_VALUE:
  case ABI_ARRAY_BOUND_RAW:
    out += t.array_bound.value;
    break;
  case ABI_ARRAY_BOUND_EXPRESSION:
    encode_expression(ctx, ctx.env.expression(t.array_bound.value), out);
    break;
  }
  out += "_";
  encode_type(ctx, t.types.front(), out);
}

void encode_type_structure(EncodeContext & ctx, const AbiType & t, string & out)
{
  switch(t.kind) {
  case ABI_TYPE_NAMED:
    emit_name_links(ctx, qualified_name_links(t.name, true), "", out);
    return;
  case ABI_TYPE_TEMPLATE_PARAMETER:
  case ABI_TYPE_TEMPLATE_PARAMETER_SPECIALIZATION:
    out += template_parameter_spelling(t.index);
    return;
  case ABI_TYPE_POINTER:
    out += "P";
    encode_type(ctx, t.types.front(), out);
    return;
  case ABI_TYPE_LVALUE_REFERENCE:
    out += "R";
    encode_type(ctx, t.types.front(), out);
    return;
  case ABI_TYPE_RVALUE_REFERENCE:
    out += "O";
    encode_type(ctx, t.types.front(), out);
    return;
  case ABI_TYPE_PACK_EXPANSION:
    out += "Dp";
    encode_type(ctx, t.types.front(), out);
    return;
  case ABI_TYPE_VENDOR_QUALIFIED:
    out += "U" + source_name(t.name);
    encode_type(ctx, t.types.front(), out);
    return;
  case ABI_TYPE_ARRAY:
    encode_array_type(ctx, t, out);
    return;
  case ABI_TYPE_BUILTIN_TRANSFORM:
    out += "u" + source_name(t.name) + "I";
    encode_type(ctx, t.types.front(), out);
    out += "E";
    return;
  case ABI_TYPE_FUNCTION:
    encode_function_type(ctx, t, out);
    return;
  case ABI_TYPE_MEMBER_POINTER:
    out += "M";
    encode_type(ctx, t.types[0], out);
    encode_type(ctx, t.types[1], out);
    return;
  case ABI_TYPE_TEMPLATE_SPECIALIZATION: {
    vector<NameLink> links = qualified_name_links(t.name, true);
    links.back().has_args = true;
    links.back().argument_refs = t.argument_refs;
    links.back().key_with_args = type_key(ctx, t);
    emit_name_links(ctx, links, "", out);
    return;
  }
  case ABI_TYPE_MEMBER:
  case ABI_TYPE_MEMBER_TEMPLATE_SPECIALIZATION:
    encode_member_type(ctx, t, out);
    return;
  case ABI_TYPE_DECLTYPE_EXPRESSION:
    out += "DT";
    encode_expression(ctx, ctx.env.expression(t.expression_ref), out);
    out += "E";
    return;
  case ABI_TYPE_LOCAL_TYPE:
    out += local_context_piece(ctx, t.context_ref);
    out += source_name(t.name);
    out += local_discriminator(t.discriminator);
    return;
  case ABI_TYPE_LAMBDA_CLOSURE:
    out += local_context_piece(ctx, t.context_ref);
    out += closure_type_piece(ctx, t.discriminator, t.types);
    return;
  default:
    throw AbiFactError("unhandled type fact kind in encoder");
  }
}

}  // namespace

void encode_type(EncodeContext & ctx, const AbiType & raw, string & out)
{
  const AbiType & t = resolve_type(ctx, raw);
  if(t.kind == ABI_TYPE_NAME_OR_REFERENCE || t.kind == ABI_TYPE_BUILTIN) {
    const char * code = builtin_type_code(t.name);
    if(code == 0) {
      throw AbiFactError("unknown builtin type '" + t.name + "'");
    }
    out += code;
    return;
  }
  if(t.kind == ABI_TYPE_STD_TEMPLATE_SPECIALIZATION) {
    encode_std_template(ctx, t, out);
    return;
  }
  if(t.kind == ABI_TYPE_CV) {
    CvFlags flags;
    const AbiType & inner = strip_cv(ctx, t, flags);
    if(!flags.is_const && !flags.is_volatile) {
      encode_type(ctx, inner, out);
      return;
    }
    const string key = cv_key(ctx, t);
    if(ctx.subs.try_emit(key, out)) {
      return;
    }
    if(flags.is_volatile) {
      out += "V";
    }
    if(flags.is_const) {
      out += "K";
    }
    encode_type(ctx, inner, out);
    ctx.subs.enter(key);
    return;
  }
  const bool substitutable = t.kind != ABI_TYPE_TEMPLATE_PARAMETER || t.substitutable;
  const string key = substitutable ? type_key(ctx, t) : string();
  if(substitutable && ctx.subs.try_emit(key, out)) {
    return;
  }
  encode_type_structure(ctx, t, out);
  if(substitutable) {
    ctx.subs.enter(key);
  }
}

namespace {

void encode_member_template_entity(EncodeContext & ctx, const AbiTemplateArgument & argument, string & out)
{
  // Matches the reference behavior for member-template template-template
  // arguments: the owner's prefix-with-arguments level is never entered, and
  // the full member-template key substitutes only at the terminal-name slot.
  vector<NameLink> links = owner_prefix_links(ctx, argument.owner_type);
  if(!links.empty() && links.back().has_args) {
    links.back().key_with_args.clear();
  }
  out += "N";
  emit_link_sequence(ctx, links, out);
  if(!ctx.subs.try_emit(argument.substitution, out)) {
    out += source_name(argument.name);
    ctx.subs.enter(argument.substitution);
  }
  out += "E";
}

void encode_entity_address(EncodeContext & ctx, const AbiTemplateArgument & argument, string & out)
{
  out += "X";
  if(argument.address_of) {
    out += "ad";
  }
  out += "L";
  if(argument.kind == ABI_TEMPLATE_ARGUMENT_ENTITY) {
    out += entity_symbol(ctx, argument.entity_ref);
  } else {
    out += argument.symbol;
  }
  out += "E";
  out += "E";
}

}  // namespace

void encode_template_argument(EncodeContext & ctx, const AbiTemplateArgument & argument, string & out)
{
  switch(argument.kind) {
  case ABI_TEMPLATE_ARGUMENT_TYPE:
    encode_type(ctx, argument.type, out);
    return;
  case ABI_TEMPLATE_ARGUMENT_VALUE:
  case ABI_TEMPLATE_ARGUMENT_UNTYPED_VALUE:
    out += integer_literal(argument.value_type.name, decimal(argument.value));
    return;
  case ABI_TEMPLATE_ARGUMENT_DEPENDENT_VALUE:
    out += "Tn";
    encode_type(ctx, argument.type, out);
    out += integer_literal(argument.value_type.name, decimal(argument.value));
    return;
  case ABI_TEMPLATE_ARGUMENT_EXPRESSION:
    out += "X";
    encode_expression(ctx, ctx.env.expression(argument.entity_ref), out);
    out += "E";
    return;
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_ENTITY: {
    const string key = argument_key(ctx, argument);
    if(ctx.subs.try_emit(key, out)) {
      return;
    }
    emit_name_links(ctx, qualified_name_links(argument.name, true), "", out);
    ctx.subs.enter(key);
    return;
  }
  case ABI_TEMPLATE_ARGUMENT_MEMBER_TEMPLATE_ENTITY:
    encode_member_template_entity(ctx, argument, out);
    return;
  case ABI_TEMPLATE_ARGUMENT_TEMPLATE_PARAMETER_TEMPLATE: {
    const string key = argument_key(ctx, argument);
    if(ctx.subs.try_emit(key, out)) {
      return;
    }
    out += template_parameter_spelling(argument.index);
    ctx.subs.enter(key);
    return;
  }
  case ABI_TEMPLATE_ARGUMENT_ENTITY:
  case ABI_TEMPLATE_ARGUMENT_EXTERNAL_ENTITY:
  case ABI_TEMPLATE_ARGUMENT_MEMBER_EXTERNAL_ENTITY:
    encode_entity_address(ctx, argument, out);
    return;
  case ABI_TEMPLATE_ARGUMENT_PACK:
    out += "J";
    for(size_t i = 0; i < argument.argument_refs.size(); ++i) {
      encode_template_argument(ctx, ctx.env.argument(argument.argument_refs[i]), out);
    }
    out += "E";
    return;
  }
  throw AbiFactError("unhandled template argument kind in encoder");
}

namespace {

void encode_simple_id(EncodeContext & ctx, const string & name, const vector<string> & argument_refs, string & out)
{
  out += source_name(name);
  if(!argument_refs.empty()) {
    encode_argument_list(ctx, argument_refs, out);
  }
}

void encode_expression_operands(EncodeContext & ctx, const AbiDependentExpression & e, size_t expected, string & out)
{
  if(e.expression_refs.size() != expected) {
    throw AbiFactError("expression fact has the wrong operand count");
  }
  for(size_t i = 0; i < expected; ++i) {
    encode_expression(ctx, ctx.env.expression(e.expression_refs[i]), out);
  }
}

}  // namespace

void encode_expression(EncodeContext & ctx, const AbiDependentExpression & e, string & out)
{
  switch(e.kind) {
  case ABI_EXPRESSION_TEMPLATE_PARAMETER:
    out += template_parameter_spelling(e.index);
    return;
  case ABI_EXPRESSION_FUNCTION_PARAMETER:
    out += e.index == 0 ? "fp_" : "fp" + decimal(static_cast<long long>(e.index - 1)) + "_";
    return;
  case ABI_EXPRESSION_LITERAL:
    out += integer_literal("int", e.text);
    return;
  case ABI_EXPRESSION_INTEGRAL_VALUE:
    out += integer_literal(e.type.name, e.text);
    return;
  case ABI_EXPRESSION_UNARY:
    out += e.op;
    encode_expression_operands(ctx, e, 1, out);
    return;
  case ABI_EXPRESSION_BINARY:
    out += e.op;
    encode_expression_operands(ctx, e, 2, out);
    return;
  case ABI_EXPRESSION_CONDITIONAL:
    out += "qu";
    encode_expression_operands(ctx, e, 3, out);
    return;
  case ABI_EXPRESSION_PACK_EXPANSION:
    out += "sp";
    encode_expression_operands(ctx, e, 1, out);
    return;
  case ABI_EXPRESSION_CALL:
    out += "cl";
    if(e.expression_refs.empty()) {
      throw AbiFactError("call expression fact needs a callee");
    }
    for(size_t i = 0; i < e.expression_refs.size(); ++i) {
      encode_expression(ctx, ctx.env.expression(e.expression_refs[i]), out);
    }
    out += "E";
    return;
  case ABI_EXPRESSION_CONVERSION:
    out += "cv";
    encode_type(ctx, e.type, out);
    encode_expression_operands(ctx, e, 1, out);
    return;
  case ABI_EXPRESSION_CAST:
    out += e.op;
    encode_type(ctx, e.type, out);
    encode_expression_operands(ctx, e, 1, out);
    return;
  case ABI_EXPRESSION_TEMPLATE_ID:
    encode_simple_id(ctx, e.text, e.argument_refs, out);
    return;
  case ABI_EXPRESSION_TYPE_TRAIT:
    out += "u" + source_name(e.text);
    for(size_t i = 0; i < e.type_arguments.size(); ++i) {
      encode_type(ctx, e.type_arguments[i], out);
    }
    out += "E";
    return;
  case ABI_EXPRESSION_SIZEOF_TYPE:
    out += "st";
    encode_type(ctx, e.type, out);
    return;
  case ABI_EXPRESSION_MEMBER:
    out += "sr";
    encode_type(ctx, e.type, out);
    if(e.close_member_owner) {
      out += "E";
    }
    encode_simple_id(ctx, e.text, e.argument_refs, out);
    return;
  case ABI_EXPRESSION_OBJECT_MEMBER:
    out += e.op;
    encode_expression_operands(ctx, e, 1, out);
    encode_simple_id(ctx, e.text, e.argument_refs, out);
    return;
  case ABI_EXPRESSION_EXTERNAL_ENTITY:
    out += "L" + e.text + "E";
    return;
  case ABI_EXPRESSION_ENTITY:
    out += "L" + entity_symbol(ctx, e.entity_ref) + "E";
    return;
  }
  throw AbiFactError("unhandled expression fact kind in encoder");
}

}  // namespace abi_mangle
