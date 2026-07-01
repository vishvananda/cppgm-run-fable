template<class T, T V>
struct constant {
  static const T value = V;
  typedef constant type;
};

template<class T>
struct remove_const {
  typedef T type;
};

template<class T>
struct remove_const<const T> {
  typedef T type;
};

template<int I>
struct int_ : constant<int, I> {
};

template<class... T>
struct plus;

template<>
struct plus<> {
  typedef constant<int, 0> type;
};

template<class T1, class... T>
struct plus<T1, T...> {
  static const int _v = T1::value + plus<T...>::type::value;
  typedef constant<typename remove_const<decltype(_v)>::type, _v> type;
};

template<class... T>
struct sum {
  typedef typename plus<T...>::type type;
};

typedef typename sum<
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1>,
    int_<1>, int_<1>, int_<1>, int_<1> >::type R;

int main()
{
  return R::value == 64 ? 0 : 1;
}
