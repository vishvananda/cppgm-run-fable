// Reduced from libstdc++ tuple constraints used by unique_ptr.
// A qualified static member-function call may be probed while checking a
// non-type SFINAE argument. If constexpr evaluation cannot prove the call,
// recovery must not reinterpret the function name as a value-bearing type.
namespace mini {

template<bool B, typename T = void>
struct __enable_if {};

template<typename T>
struct __enable_if<true, T> { typedef T type; };

template<bool B, typename T = void>
using __enable_if_t = typename __enable_if<B, T>::type;

template<bool V>
struct __bool_constant {
  static constexpr bool value = V;
};

template<typename T, typename...>
using __first_t = T;

template<bool B, typename T = void>
struct __enable_if_bool {};

template<typename T>
struct __enable_if_bool<true, T> { typedef T type; };

template<typename... Bn>
__first_t<__bool_constant<true>,
          typename __enable_if_bool<bool(Bn::value), void>::type...>
__and_fn(int);

template<typename... Bn>
__bool_constant<false> __and_fn(...);

template<typename... Bn>
struct __and_ : decltype(__and_fn<Bn...>(0)) {};

template<typename T>
struct __is_implicitly_default_constructible
    : __bool_constant<true> {};

template<bool, typename... Types>
struct _TupleConstraints {
  static constexpr bool __is_implicitly_default_constructible()
  {
    return __and_<mini::__is_implicitly_default_constructible<Types>...>::value;
  }
};

template<typename... Types>
struct tuple;

template<typename T1, typename T2>
struct tuple<T1, T2> {
  template<bool Dummy, typename U1, typename U2>
  using _ImplicitDefaultCtor = __enable_if_t<
      _TupleConstraints<Dummy, U1, U2>::__is_implicitly_default_constructible(),
      bool>;

  template<bool Dummy = true, _ImplicitDefaultCtor<Dummy, T1, T2> = true>
  tuple(int) {}

  tuple(long) {}
};

}

struct Owner { struct Node; };

struct Use {
  mini::tuple<Owner::Node *, int> value;
  Use() : value(0) {}
};

struct Owner::Node {};

int main()
{
  Use use;
  (void)use;
  return 0;
}
