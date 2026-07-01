// VALIDATION: compile-pass
// N3485 focus: 14.2 [temp.names], 14.8.1 [temp.arg.explicit], 14.8.2 [temp.deduct]

namespace std {
struct identity {};

template<class T, class Proj, int = 0>
const T * __find(const T * first, const T * last, const T &, Proj &)
{
  return last;
}
}

typedef unsigned long size_t;

const char16_t * g(const char16_t * s, size_t n, const char16_t & a)
{
  std::identity proj;
  return std::__find(s, s + n, a, proj);
}

int main()
{
  const char16_t text[] = u"x";
  return g(text, 1, text[0]) == text + 1 ? 0 : 1;
}
