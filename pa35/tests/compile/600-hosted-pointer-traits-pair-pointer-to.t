#include <memory>
#include <string>
#include <utility>
#include <type_traits>
static_assert(std::is_same<std::pointer_traits<int*>::element_type, int>::value, "pointer_traits element_type");
struct AliasTemplateDecl {};
struct ClassTemplateDecl {};
typedef std::pair<const std::string, AliasTemplateDecl *> AliasPair;
typedef std::pair<const std::string, ClassTemplateDecl *> ClassPair;
bool pointer_traits_pair_anchor()
{
  AliasPair alias_pair = AliasPair(std::string("a"), (AliasTemplateDecl *)0);
  ClassPair class_pair = ClassPair(std::string("c"), (ClassTemplateDecl *)0);
  return std::pointer_traits<const AliasPair *>::pointer_to(alias_pair) == &alias_pair &&
         std::pointer_traits<const ClassPair *>::pointer_to(class_pair) == &class_pair;
}
static_assert(sizeof(&pointer_traits_pair_anchor) > 0, "pointer_traits pointer_to body anchor");
