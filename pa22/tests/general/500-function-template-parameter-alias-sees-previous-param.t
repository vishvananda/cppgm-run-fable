// VALIDATION: compile-pass
// Reduced from Boost.Asio's buffer sequence traits. A function template
// parameter can name an earlier parameter inside a dependent alias-template
// SFINAE type while the function signature is instantiated.

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

struct value {};

template <typename T>
char helper(T *t, enable_if_t<is_same<decltype(*t), T &>::value> * = 0);

int main()
{
  return sizeof(helper<value>((value *)0, 0)) == sizeof(char) ? 0 : 1;
}
