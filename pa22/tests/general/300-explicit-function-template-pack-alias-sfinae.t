template<class T, T v>
struct integral_constant {
  static const T value = v;
};

template<bool B>
struct bool_constant : integral_constant<bool, B> {};

typedef bool_constant<true> true_type;
typedef bool_constant<false> false_type;

struct AlwaysTrue : true_type {};

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class...>
using expand_to_true = true_type;

template<class... Pred>
expand_to_true<enable_if_t<Pred::value>...> and_helper(int);

template<class...>
false_type and_helper(...);

template<class... Pred>
using And = decltype(and_helper<Pred...>(0));

int main()
{
  And<AlwaysTrue>* p = 0;
  (void)p;
  return 0;
}
