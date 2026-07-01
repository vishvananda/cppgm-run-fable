// VALIDATION: compile-pass
// Reduced from Boost.Asio's buffer sequence traits. A dependent alias-template
// SFINAE argument contains decltype of a qualified function-template call; the
// stored argument syntax must keep the declaration-order source location of
// that call when the helper signature is instantiated.

namespace ns
{
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

  struct selected {};

  template<class T>
  selected select(T &);

  namespace detail
  {
    template<typename T>
    char (&helper(T *t,
                  enable_if_t<!is_same<decltype(ns::select(*t)), void>::value>
                      *))[2];
  }
}

struct input {};

int main()
{
  return sizeof(ns::detail::helper<input>(0, 0)) == 2 ? 0 : 1;
}
