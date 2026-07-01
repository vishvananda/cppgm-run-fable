// VALIDATION: compile-pass
// A function-template overload whose defaulted non-type parameter type
// substitutes to enable_if<false>::type should be SFINAE-dropped, leaving the
// ordinary overload viable.

template<bool, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

unsigned long convert(unsigned long v)
{
  return v;
}

template<class Fp, enable_if_t<(sizeof(Fp) == sizeof(int)), int> = 0>
long long convert(Fp v)
{
  return (long long)v;
}

int main()
{
  return convert(7ul) == 7ul ? 0 : 1;
}
