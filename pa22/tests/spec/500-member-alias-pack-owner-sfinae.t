// VALIDATION: compile-pass
// N3485 focus: 14.5.7 [temp.alias], 14.8.2 [temp.deduct],
// 14.8.2.5 [temp.deduct.type]
// A member alias template may name another member template through the current
// class-template owner. Substitution must bind that owner and preserve nested
// pack syntax when the alias is used in an enable-if style non-type argument.

template<bool B, class T = int>
struct Enable
{};

template<class T>
struct Enable<true, T>
{
  typedef T type;
};

template<class... Cond>
struct And;

template<>
struct And<>
{
  static const bool value = true;
};

template<class C>
struct And<C>
{
  static const bool value = C::value;
};

template<class... Cond>
using Require = typename Enable<And<Cond...>::value, int>::type;

struct Good
{
  static const bool value = true;
};

struct Alloc
{};

struct Widget
{
  Widget(int &, long &) {}
};

template<class Allocator>
struct Traits
{
  template<class T, class... Args>
  struct Helper
  {
    typedef Good type;
  };

  template<class T, class... Args>
  using Has =
      typename Traits<Allocator>::template Helper<T, Args...>::type;

  template<class T, class... Args>
  static Require<Has<T, Args...>> construct(Allocator &, T *, Args &&...)
  {
    return 7;
  }
};

int main()
{
  Alloc alloc;
  Widget * ptr = 0;
  int i = 1;
  long l = 2;
  return Traits<Alloc>::construct(alloc, ptr, i, l) == 7 ? 0 : 1;
}
