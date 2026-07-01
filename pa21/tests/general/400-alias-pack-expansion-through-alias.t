template<class A, class B>
struct Same {
  static const bool value = false;
};

template<class A>
struct Same<A, A> {
  static const bool value = true;
};

template<class... Bn>
struct Or {
  static const bool value = false;
};

template<class B1>
struct Or<B1> {
  static const bool value = B1::value;
};

template<class B1, class B2, class... Bn>
struct Or<B1, B2, Bn...> {
  static const bool value = B1::value || Or<B2, Bn...>::value;
};

template<class Tp, class... Types>
using IsOneOf = Or<Same<Tp, Types>...>;

template<class Tp>
using IsSigned = IsOneOf<Tp, signed char, short, int, long, long long>;

template<class Tp>
using IsUnsigned = IsOneOf<Tp, unsigned char, unsigned short, unsigned int,
                           unsigned long, unsigned long long>;

template<class Tp>
using IsStandardInteger = Or<IsSigned<Tp>, IsUnsigned<Tp> >;

int main() {
  return IsStandardInteger<int>::value ? 0 : 1;
}
