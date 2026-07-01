// VALIDATION: compile-pass
// N3485 focus: 12.3.2 [class.conv.fct], 14.8.2.3 [temp.deduct.conv]

struct traits
{
};

template<class CharT, class Traits>
struct view
{
  const CharT *p;
  unsigned long n;

  view()
    : p(0), n(0)
  {
  }

  view(const view &rhs)
    : p(rhs.p), n(rhs.n)
  {
  }

  view(const CharT *ptr, unsigned long len)
    : p(ptr), n(len)
  {
  }
};

template<class CharT, class Traits>
struct string_like
{
  CharT buffer[4];

  string_like()
    : buffer()
  {
  }

  template<template<class, class> class View>
  explicit string_like(View<CharT, Traits> v)
  {
    for(unsigned long i = 0; i < 4; ++i) {
      buffer[i] = v.p[i];
    }
  }

  const CharT *data() const
  {
    return buffer;
  }

  unsigned long size() const
  {
    return 4;
  }

  template<template<class, class> class View>
  operator View<CharT, Traits>() const
  {
    return View<CharT, Traits>(data(), size());
  }

  template<template<class, class> class View>
  string_like &operator=(View<CharT, Traits> v)
  {
    for(unsigned long i = 0; i < 4; ++i) {
      buffer[i] = v.p[i];
    }
    return *this;
  }
};

int main()
{
  string_like<char, traits> s;
  view<char, traits> v(s);
  view<char, traits> v2;
  v2 = s;

  view<char, traits> source("abcd", 4);
  string_like<char, traits> s2(source);
  string_like<char, traits> s3;
  s3 = source;

  return v.n == 4 &&
         v2.n == 4 &&
         s2.data()[0] == 'a' &&
         s2.data()[3] == 'd' &&
         s3.data()[1] == 'b' &&
         s3.data()[2] == 'c' ? 0 : 1;
}
