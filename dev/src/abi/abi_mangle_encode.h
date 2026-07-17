#pragma once

// Internal encoding interfaces shared by the PA30 abimangle implementation.
// The public data model and API live in abi_mangle.h; everything here is an
// implementation detail of the encoder split across abi_mangle_encode.cpp
// (types, template arguments, expressions) and abi_mangle_names.cpp
// (qualified names, function encodings, targets).

#include "abi_mangle.h"

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace abi_mangle {

class AbiFactError : public std::runtime_error
{
public:
  explicit AbiFactError(const std::string & message)
    : std::runtime_error(message)
  {
  }
};

// One Itanium substitution table per mangled name. Components are identified
// by canonical structural keys (or explicit keys carried by the facts); equal
// keys share one substitution slot.
class SubstitutionTable
{
public:
  bool try_emit(const std::string & key, std::string & out) const;
  bool contains(const std::string & key) const;
  void enter(const std::string & key);

private:
  std::map<std::string, std::size_t> index_by_key_;
};

// Per-case fact environment: definition ids to their records.
struct CaseEnvironment
{
  std::map<std::string, const AbiType *> types;
  std::map<std::string, const AbiTemplateArgument *> arguments;
  std::map<std::string, const AbiDependentExpression *> expressions;
  std::map<std::string, const AbiLocalContext *> contexts;
  std::map<std::string, const AbiEntityFact *> entities;

  const AbiType & type(const std::string & id) const;
  const AbiTemplateArgument & argument(const std::string & id) const;
  const AbiDependentExpression & expression(const std::string & id) const;
  const AbiLocalContext & context(const std::string & id) const;
  const AbiEntityFact & entity(const std::string & id) const;
};

struct EncodeContext
{
  explicit EncodeContext(const CaseEnvironment & env_in) : env(env_in) {}

  const CaseEnvironment & env;
  SubstitutionTable subs;
};

// One component of a possibly-nested Itanium name. Links either carry a
// precomputed spelling (source names, terminals, closure pieces) or a type
// fact spelled through encode_type (dependent owners such as T_ or
// builtin-transform prefixes).
struct NameLink
{
  std::string spelled;
  const AbiType * type_piece = nullptr;
  std::string suffix;  // ABI tags, emitted after the name, before arguments
  std::string key;
  std::string key_with_args;
  bool std_abbrev = false;
  bool has_args = false;
  std::vector<std::string> argument_refs;
};

// abi_mangle_encode.cpp
const char * builtin_type_code(const std::string & name);
std::string source_name(const std::string & name);
std::string integer_literal(const std::string & type_name, const std::string & value);
const AbiType & resolve_type(const EncodeContext & ctx, const AbiType & type);
std::string type_key(const EncodeContext & ctx, const AbiType & type);
std::string argument_key(const EncodeContext & ctx, const AbiTemplateArgument & argument);
std::string expression_key(const EncodeContext & ctx, const AbiDependentExpression & expression);
void encode_type(EncodeContext & ctx, const AbiType & type, std::string & out);
void encode_template_argument(EncodeContext & ctx, const AbiTemplateArgument & argument, std::string & out);
void encode_argument_list(EncodeContext & ctx, const std::vector<std::string> & argument_refs, std::string & out);
void encode_expression(EncodeContext & ctx, const AbiDependentExpression & expression, std::string & out);
void emit_name_links(EncodeContext & ctx, const std::vector<NameLink> & links, const std::string & qualifiers, std::string & out);
std::vector<NameLink> qualified_name_links(const std::string & qualified_name, bool keyed);

// abi_mangle_names.cpp
CaseEnvironment build_case_environment(const AbiFactCase & fact_case);
std::string local_context_piece(EncodeContext & ctx, const std::string & context_ref);
std::string closure_type_piece(EncodeContext & ctx, const std::string & number, const std::vector<AbiType> & signature_types);
std::string local_discriminator(const std::string & occurrence);
std::string entity_symbol(const EncodeContext & ctx, const std::string & entity_ref);
std::string mangle_fact_case(const AbiFactCase & fact_case);

}  // namespace abi_mangle
