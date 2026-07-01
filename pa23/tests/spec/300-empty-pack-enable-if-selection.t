// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.3 [temp.over]

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class Alloc, class Ptr, class... Args>
struct has_construct
{
  static const bool value = false;
};

template<>
struct has_construct<int, int*>
{
  static const bool value = true;
};

template<class Alloc>
struct traits
{
  template<class Tp, class... Args, enable_if_t<has_construct<Alloc, Tp*, Args...>::value, int> = 0>
  static int construct(Alloc&, Tp*, Args&&...) { return 1; }

  template<class Tp, class... Args, enable_if_t<!has_construct<Alloc, Tp*, Args...>::value, int> = 0>
  static int construct(Alloc&, Tp*, Args&&...) { return 2; }
};

int main()
{
  int alloc = 0;
  int value = 0;
  return traits<int>::construct(alloc, &value) - 1;
}
