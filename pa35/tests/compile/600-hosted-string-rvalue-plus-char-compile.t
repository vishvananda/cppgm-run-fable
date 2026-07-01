#include <string>
#include <utility>
#include <type_traits>
static_assert(std::is_same<decltype(std::declval<std::string>() + std::declval<char>()), std::string>::value,
              "std::string + char -> std::string");
std::string string_plus_char_anchor()
{
  std::string delimiter = "";
  char quote = '"';
  return std::string(")") + delimiter + quote;
}
static_assert(sizeof(&string_plus_char_anchor) > 0, "string plus char body anchor");
