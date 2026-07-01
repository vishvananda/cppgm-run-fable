template<class T, T v>
struct integral_constant {
  static const T value = v;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<bool B>
using BoolConstant = integral_constant<bool, B>;

template<class...>
using expand_to_true = true_type;

template<class... Pred>
expand_to_true<enable_if_t<Pred::value>...> and_helper(int);

template<class...>
false_type and_helper(...);

template<class... Pred>
using And = decltype(and_helper<Pred...>(0));

template<class...>
struct LaterPredicate;

template<class... Tp>
struct Box {
  template<class... Up,
           enable_if_t<And<BoolConstant<sizeof...(Up) == sizeof...(Tp)>,
                           LaterPredicate<Up...> >::value,
                       int> = 0>
  static int select(Up&&...)
  {
    return sizeof(typename LaterPredicate<Up...>::type);
  }

  static int select(...)
  {
    return 0;
  }
};

int main()
{
  return Box<int, int, int>::select();
}
