template<bool B>
struct integral_constant {
  static constexpr bool value = B;
};

template<bool B>
using bool_constant = integral_constant<B>;

template<class T>
using remove_cv_t = __remove_cv(T);

template<class T>
using remove_ref_t = __remove_reference_t(T);

template<class T>
using decay_t = __decay(T);

template<class T>
using lref_t = __add_lvalue_reference(T);

template<class T>
using rref_t = __add_rvalue_reference(T);

template<class T>
struct is_void : bool_constant<__is_same(__remove_cv(T), void)> {};

template<class T, class U>
using same_t = bool_constant<__is_same(T, U)>;

template<class T>
struct trait_pack {
  static constexpr bool a = __is_trivially_destructible(T);
  static constexpr bool b = __is_destructible(T);
  static constexpr bool c = __is_integral(T);
  static constexpr bool d = __is_trivially_constructible(T);
  static constexpr bool e = __is_trivially_copyable(T);
  static constexpr bool f = __is_empty(T);
};

template<class T>
struct cached_alias {
  typedef lref_t<const T> type;
};

template<class T>
struct is_copy_assignable : bool_constant<__is_assignable(lref_t<T>, lref_t<const T>)> {};

template<class T, class... Args>
struct is_constructible : bool_constant<__is_constructible(T, Args...)> {};

template<class T, class U>
struct is_convertible : bool_constant<__is_convertible(T, U)> {};

struct Empty {};

static_assert(is_void<void>::value, "");
static_assert(same_t<char, char>::value, "");
static_assert(trait_pack<char>::a, "");
static_assert(trait_pack<char>::b, "");
static_assert(trait_pack<char>::c, "");
static_assert(trait_pack<char>::d, "");
static_assert(trait_pack<char>::e, "");
static_assert(trait_pack<Empty>::f, "");
static_assert(is_copy_assignable<unsigned long>::value, "");
static_assert(is_constructible<unsigned long, unsigned long>::value, "");
static_assert(is_convertible<int, int>::value, "");

int main() {
  return 0;
}
