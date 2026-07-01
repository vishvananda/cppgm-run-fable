template<long V>
struct value_constant
{
  static constexpr long value = V;
};

template<long V>
struct abs_value : value_constant<(V < 0 ? -V : V)>
{
};

template<long A, long B>
struct gcd_value : gcd_value<B, A % B>
{
};

template<long A>
struct gcd_value<A, 0> : abs_value<A>
{
};

template<long N, long D = 1>
struct ratio
{
  static constexpr long num = N / gcd_value<N, D>::value;
  static constexpr long den = abs_value<D>::value / gcd_value<N, D>::value;
  typedef ratio<num, den> type;
};

template<long N, long D>
constexpr long ratio<N, D>::num;

template<long N, long D>
constexpr long ratio<N, D>::den;

typedef typename ratio<1, 1000000000>::type normalized;

static_assert(normalized::den == 1000000000, "out-of-class constexpr member preserves non-bool value");

int main()
{
  return 0;
}
