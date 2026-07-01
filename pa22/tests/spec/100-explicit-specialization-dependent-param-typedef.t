// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec], 14.8.2 [temp.deduct]

struct traits_float
{
  typedef unsigned carrier_uint;
};

struct result
{
  char * ptr;
  int ec;
};

template<class T, class Traits>
result convert(typename Traits::carrier_uint value, int exponent, char * first, char * last)
{
  return {first + value + exponent, last == 0};
}

template<>
result convert<float, traits_float>(unsigned value, int exponent, char * first, char * last)
{
  return {last + value + exponent, 0};
}

int main()
{
  char buf[8];
  result r = convert<float, traits_float>(1, 2, buf, buf + 8);
  return r.ptr == buf + 11 ? 0 : 1;
}
