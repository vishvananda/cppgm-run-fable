// VALIDATION: compile-pass
// N3485 focus: 14.5.1 [temp.class], 7.3.1.1 [namespace.unnamed]

namespace outer {
namespace detail {
namespace {

template<class T>
struct holder {
  static int value;
};

template<class T>
int holder<T>::value = 4;

}  // namespace
}  // namespace detail
}  // namespace outer

int main()
{
  return outer::detail::holder<int>::value == 4 ? 0 : 1;
}
