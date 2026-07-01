// VALIDATION: compile-pass
// N3485 focus: 14.8.1 [temp.arg.explicit], 14.8.2 [temp.deduct], 14.8.3 [temp.over]

namespace std {
inline namespace __1 {

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

struct _classic_alg_policy {};

template<class I>
struct has_cat
{
  enum { value = 0 };
};

template<>
struct has_cat<const char*>
{
  enum { value = 1 };
};

template<class P>
struct _iter_ops
{
  template<class I>
  using __difference_type = long;
};

template<class P, class I, class O, enable_if_t<has_cat<I>::value, int> = 0>
int __copy_n(I, typename _iter_ops<P>::template __difference_type<I>, O)
{
  return 1;
}

template<class P, class I, class O, enable_if_t<!has_cat<I>::value, int> = 0>
int __copy_n(I, typename _iter_ops<P>::template __difference_type<I>, O)
{
  return 2;
}

}  // namespace __1
}  // namespace std

int main()
{
  return std::__copy_n<std::_classic_alg_policy>((const char*)0, 1, (char*)0) - 1;
}
