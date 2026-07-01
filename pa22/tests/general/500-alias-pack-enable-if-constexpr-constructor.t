template<bool Value>
struct bool_constant {
  static constexpr bool value = Value;
};

using true_type = bool_constant<true>;

template<bool Cond, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  using type = T;
};

template<bool Cond, class T = void>
using enable_if_t = typename enable_if<Cond, T>::type;

template<class... Cond>
struct and_;

template<>
struct and_<> : true_type {};

template<class Cond>
struct and_<Cond> : Cond {};

template<class Lhs, class Rhs>
struct and_<Lhs, Rhs> : bool_constant<Lhs::value && Rhs::value> {};

template<class... Cond>
using Require = enable_if_t<and_<Cond...>::value>;

template<class From, class To>
struct is_convertible : true_type {};

template<class Rep>
struct duration_values;

template<>
struct duration_values<long long> {
  static constexpr long long zero() { return 0; }
  static constexpr long long min() { return -1; }
};

template<class Rep>
class duration {
public:
  using rep = Rep;

  constexpr duration() = default;

  template<class Rep2, class = Require<is_convertible<const Rep2 &, rep>, true_type> >
  constexpr explicit duration(const Rep2 & value) : r(value) {}

  constexpr rep count() const { return r; }

  static constexpr duration zero() { return duration(duration_values<rep>::zero()); }
  static constexpr duration min() { return duration(duration_values<rep>::min()); }

private:
  rep r;
};

static_assert(duration<long long>::min().count() < duration<long long>::zero().count(), "");

int main()
{
  return 0;
}
