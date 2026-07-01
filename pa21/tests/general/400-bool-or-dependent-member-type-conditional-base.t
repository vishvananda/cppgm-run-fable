namespace boost {

template<bool B, class T, class F>
struct conditional {
  typedef T type;
};

template<class T, class F>
struct conditional<false, T, F> {
  typedef F type;
};

template<class T, T val>
struct integral_constant {
  static const T value = val;
};

template<class T, T val>
T const integral_constant<T, val>::value;

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<class A, class B>
struct is_same : false_type {
};

template<class A>
struct is_same<A, A> : true_type {
};

template<class T>
struct identity {
  typedef T type;
};

template<class T>
struct yes : true_type {
};

struct none_t {
};

namespace detail {

template<class T>
struct trait
  : boost::conditional<boost::yes<typename boost::identity<T>::type>::value ||
                           boost::is_same<typename boost::identity<T>::type, none_t>::value,
                       boost::true_type,
                       boost::false_type>::type {
};

}

template<class...>
struct disjunction : false_type {
};

template<class T>
struct disjunction<T> : T {
};

template<class T, class... U>
struct disjunction<T, U...>
  : boost::conditional<bool(T::value), T, disjunction<U...> >::type {
};

}

int main()
{
  return boost::disjunction<boost::detail::trait<int>,
                            boost::false_type>::value ? 0 : 1;
}
