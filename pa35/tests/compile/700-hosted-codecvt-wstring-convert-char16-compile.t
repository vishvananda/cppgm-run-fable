#include <codecvt>
#include <locale>
#include <type_traits>
static_assert(std::is_same<std::codecvt_utf8<char16_t>::intern_type, char16_t>::value, "codecvt_utf8 intern_type");
void codecvt_wstring_convert_anchor()
{
  std::wstring_convert<std::codecvt_utf8<char16_t>, char16_t> test;
  (void)test;
}
static_assert(sizeof(&codecvt_wstring_convert_anchor) > 0, "wstring_convert body anchor");
