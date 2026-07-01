#pragma once

// Shared symbol facts carried by LowIR and MIR model scaffolds.
//
// This is intentionally smaller than the semantic-layer symbol representation:
// by the time a program crosses the LowIR boundary, backend code should only
// need concrete internal/object spellings and linkage choices.

#include <string>

namespace ir_model {

enum SymbolLinkage
{
  SL_INTERNAL,
  SL_EXTERNAL,
  SL_WEAK
};

struct ExportedSymbol
{
  std::string internal_symbol;
  std::string object_symbol;
  std::string thread_local_wrapper_object_symbol;
  bool keep_internal_alias = false;
  bool prefer_local_object_binding = false;
  SymbolLinkage linkage = SL_EXTERNAL;
};

inline bool has_object_symbol(const ExportedSymbol & symbol)
{
  return !symbol.object_symbol.empty();
}

inline bool has_exported_object_symbol(const ExportedSymbol & symbol)
{
  return !symbol.object_symbol.empty() && symbol.linkage != SL_INTERNAL;
}

inline std::string exported_object_symbol(const ExportedSymbol & symbol)
{
  return symbol.object_symbol;
}

inline bool has_weak_linkage(const ExportedSymbol & symbol)
{
  return symbol.linkage == SL_WEAK;
}

}  // namespace ir_model
