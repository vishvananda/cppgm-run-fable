#include <functional>
#include <string>

struct Existing
{
  std::function<std::string()> thunk;
};

std::string make_string()
{
  Existing existing;
  existing.thunk = []() -> std::string { return std::string("x"); };
  return existing.thunk();
}

struct TypeTraitCallbacks
{
  std::function<int(const std::string &)> class_info_for_type;
};

struct NothrowCallbacks
{
  std::function<TypeTraitCallbacks()> make_type_trait_callbacks;
};

struct Analyzer
{
  TypeTraitCallbacks type_trait_callbacks() const
  {
    TypeTraitCallbacks callbacks;
    callbacks.class_info_for_type =
        [this](const std::string &) -> int
        {
          return 0;
        };
    return callbacks;
  }

  NothrowCallbacks nothrow_callbacks()
  {
    NothrowCallbacks callbacks;
    callbacks.make_type_trait_callbacks =
        [this]() -> TypeTraitCallbacks
        {
          return type_trait_callbacks();
        };
    return callbacks;
  }
};

int function_nullary_reentry_anchor()
{
  make_string();
  Analyzer analyzer;
  NothrowCallbacks callbacks = analyzer.nothrow_callbacks();
  TypeTraitCallbacks traits = callbacks.make_type_trait_callbacks();
  return traits.class_info_for_type("x");
}
static_assert(sizeof(&function_nullary_reentry_anchor) > 0, "std::function reentry body anchor");
