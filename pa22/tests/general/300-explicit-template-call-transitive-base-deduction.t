namespace ns {

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<unsigned long I, class... Ts>
struct impl {};

template<unsigned long I, class Head, class... Tail>
struct impl<I, Head, Tail...> : impl<I + 1, Tail...> {};

template<class... Ts>
struct tuple : impl<0, Ts...> {};

template<unsigned long I, class Head, class... Tail>
Head& helper(impl<I, Head, Tail...>&) { return *static_cast<Head*>(0); }

template<unsigned long I, class Head, class... Tail>
const Head& helper(const impl<I, Head, Tail...>&) {
  return *static_cast<const Head*>(0);
}

template<unsigned long I, class... Types>
enable_if_t<(I >= sizeof...(Types))>
helper(const tuple<Types...>&) = delete;

}

int main() {
  ns::tuple<int*, int> t;
  (void)ns::helper<1>(t);
  return 0;
}
