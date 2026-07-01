// VALIDATION: compile-pass
// A later ordinary declaration must not hide a hidden friend found by ADL while
// probing an expression-SFINAE trait.

template <bool B, class T = void> struct enable_if {};
template <class T> struct enable_if<true, T> { typedef T type; };

template <class T> T&& declval();

template <class...> struct make_void { typedef void type; };
template <class... T> using void_t = typename make_void<T...>::type;

template <bool B> struct bool_constant { static constexpr bool value = B; };
typedef bool_constant<true> true_type;
typedef bool_constant<false> false_type;

namespace associated {

struct arg {};

struct property
{
  friend int prefer(arg, property);
};

} // namespace associated

namespace traits {

template <class T, class U, class = void>
struct prefer_free : false_type
{
};

template <class T, class U>
struct prefer_free<T, U, void_t<decltype(prefer(declval<T>(), declval<U>()))> >
    : true_type
{
};

} // namespace traits

struct prefer_object
{
  template <class T, class U>
  typename enable_if<traits::prefer_free<T, U>::value, int>::type
  operator()(T, U) const;
};

extern prefer_object prefer;

static_assert(traits::prefer_free<associated::arg, associated::property>::value,
    "ADL hidden friend should not be shadowed by a later ordinary value");

int main()
{
  return 0;
}
