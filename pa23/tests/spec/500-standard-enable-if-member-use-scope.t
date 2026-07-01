// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.2.5 [temp.deduct.type]
// The arguments of a qualified std::enable_if<...>::type are parsed at the
// source use site, not in namespace std. Boost.Exception uses this form with a
// type trait argument that names an enclosing namespace's helper type.

namespace std
{
template<bool B, class T = void>
struct enable_if
{};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<class B, class D>
struct is_base_of
{
  static const bool value = true;
};
}

namespace outer
{
namespace detail
{
struct encoder
{};
}

namespace helpers
{
template<class Encoder, class T>
typename std::enable_if<
    std::is_base_of<detail::encoder, Encoder>::value,
    int>::type
serialize(Encoder &, T const &)
{
  return 7;
}
}

template<class T>
int call()
{
  detail::encoder enc;
  using namespace helpers;
  return serialize(enc, T());
}
}

int main()
{
  return outer::call<int>() == 7 ? 0 : 1;
}
