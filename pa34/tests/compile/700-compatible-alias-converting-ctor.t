// VALIDATION: compile-pass
// Boost.Regex shared_ptr<implementation> to shared_ptr<named_subexpressions>
// reduction: libc++ wraps __is_convertible in an _Or alias before using it as
// the enable_if condition on the converting constructor.

template<class T, T v>
struct integral_constant
{
  static const T value = v;
};

typedef integral_constant<bool, false> false_type;

template<bool>
struct OrImpl;

template<>
struct OrImpl<true>
{
  template<class Res, class First, class... Rest>
  using Result =
      typename OrImpl<!bool(First::value) && sizeof...(Rest) != 0>::template Result<First, Rest...>;
};

template<>
struct OrImpl<false>
{
  template<class Res, class...>
  using Result = Res;
};

template<class... Args>
using Or = typename OrImpl<sizeof...(Args) != 0>::template Result<false_type, Args...>;

template<bool B, class T = void>
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

template<class From, class To>
struct is_convertible : integral_constant<bool, __is_convertible(From, To)>
{
};

template<class Y, class T>
struct compatible_with : Or<is_convertible<Y *, T *> >
{
};

struct Base
{
};

template<class T>
struct Middle : Base
{
};

template<class T>
struct Derived : Middle<T>
{
};

template<class T>
struct Ptr
{
  Ptr()
  {
  }

  template<class U, enable_if_t<compatible_with<U, T>::value, int> = 0>
  Ptr(const Ptr<U> &)
  {
  }
};

template<class X>
struct Holder
{
  Ptr<Derived<X> > p;

  Ptr<Base> get() const
  {
    return p;
  }
};

int main()
{
  Holder<int> h;
  Ptr<Base> p = h.get();
  return 0;
}
