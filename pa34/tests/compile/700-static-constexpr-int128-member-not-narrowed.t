template <class T>
struct limits_like {
  static constexpr bool is_signed = T(-1) < T(0);
  static constexpr int digits = static_cast<int>(sizeof(T) * __CHAR_BIT__ - is_signed);
  static constexpr T min_value = is_signed ? T(T(1) << digits) : T(0);
  static constexpr T max_value = is_signed ? T(T(~0) ^ min_value) : T(~0);

  static constexpr T max()
  {
    return max_value;
  }
};

int main()
{
  __int128_t max_value = limits_like<__int128_t>::max();
  __int128_t ull_max =
      static_cast<__int128_t>(18446744073709551615ULL);
  if(!(max_value > ull_max)) {
    return 1;
  }

  unsigned long long high =
      static_cast<unsigned long long>(static_cast<__uint128_t>(max_value) >> 64);
  return high == 9223372036854775807ULL ? 0 : 2;
}
