// VALIDATION: compile-pass
// N3485 focus: 7.1.5 [dcl.constexpr], 9.4.2 [class.static.data]

struct uint128
{
  unsigned long high;
  unsigned long low;

  constexpr uint128() : high(), low() {}
  constexpr uint128(const uint128 &) = default;
  constexpr uint128(unsigned long high_, unsigned long low_)
    : high(high_), low(low_)
  {}
};

template<bool enabled>
struct cache_holder
{
  static constexpr uint128 cache[] = {{1, 2}, {3, 4}};
};

template<bool enabled>
constexpr uint128 cache_holder<enabled>::cache[];

int main()
{
  return cache_holder<true>::cache[1].low == 4 ? 0 : 1;
}
