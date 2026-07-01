template<class T>
using decay_t = T;

template<class... Ts>
struct tuple {
};

tuple<int> seed;

template<class F, class... BoundArgs>
struct bind {
  typedef tuple<decay_t<BoundArgs>...> tuple_type;
};

template<class F, class... BoundArgs>
struct outer {
  typedef typename bind<F, BoundArgs...>::tuple_type type;
};

outer<int, int, char>::type value;

int main() {
  return 0;
}
