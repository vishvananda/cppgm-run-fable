namespace lib {
namespace detail {
struct false_t {};
}

template<class T, class U = T, class O = detail::false_t>
struct wrapper {
  static const int value = 0;
};

template<class T, class U>
struct wrapper<T, U, detail::false_t> {
  static const int value = 1;
};

template<class T>
struct wrapper<T, T, detail::false_t> {
  static const int value = 2;
};

template<class T>
struct box : wrapper<box<T> > {};
}

static_assert(lib::box<int>::value == 2, "");

int main()
{
  return lib::box<int>::value == 2 ? 0 : 1;
}
