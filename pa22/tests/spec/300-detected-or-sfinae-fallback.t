template<class A, class B>
struct is_same {
  static const bool value = false;
};

template<class A>
struct is_same<A, A> {
  static const bool value = true;
};

template<class T>
using pointer_member = typename T::pointer;

template<class...>
using void_t = void;

template<class Default, class Void, template<class...> class Op, class... Args>
struct detector {
  using type = Default;
};

template<class Default, template<class...> class Op, class... Args>
struct detector<Default, void_t<Op<Args...>>, Op, Args...> {
  using type = Op<Args...>;
};

template<class Default, template<class...> class Op, class... Args>
using detected_or_t = typename detector<Default, void, Op, Args...>::type;

template<class T>
struct Missing {};

template<class T>
struct Present {
  using pointer = T *;
};

using P = detected_or_t<int *, pointer_member, Missing<int>>;
using Q = detected_or_t<int *, pointer_member, Present<int>>;

static_assert(is_same<P, int *>::value, "");
static_assert(is_same<Q, int *>::value, "");

int main() {
  return 0;
}
