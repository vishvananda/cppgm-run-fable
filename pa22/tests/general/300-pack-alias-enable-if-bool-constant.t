// VALIDATION: compile-pass

template<bool Value>
struct bool_constant
{
  static constexpr bool value = Value;
};

using true_type = bool_constant<true>;

template<bool Cond, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  using type = T;
};

template<bool Cond, class T = void>
using enable_if_t = typename enable_if<Cond, T>::type;

template<class... Cond>
struct and_;

template<>
struct and_<> : true_type
{
};

template<class Lhs, class Rhs>
struct and_<Lhs, Rhs> : bool_constant<Lhs::value && Rhs::value>
{
};

template<class... Cond>
using Require = enable_if_t<and_<Cond...>::value>;

template<class T>
struct inherited_true : true_type
{
};

template<class T, class = Require<inherited_true<T>, true_type> >
struct box
{
  static constexpr int value = 7;
};

static_assert(box<int>::value == 7, "");

int main()
{
  return 0;
}
