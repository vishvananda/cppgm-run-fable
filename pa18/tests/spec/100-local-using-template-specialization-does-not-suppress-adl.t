// VALIDATION: compile-pass
// N3485 focus: 3.4.2 [basic.lookup.argdep], 7.3.3 [namespace.udecl]

namespace boost_no_using_breaks_adl
{

namespace lib
{

template<class T>
T * get_pointer(T * p)
{
  return p;
}

namespace inner
{

template<class T>
struct X
{
};

template<class T>
T * get_pointer(X<T>)
{
  return 0;
}

}  // namespace inner

}  // namespace lib

namespace user
{

template<class T>
int f(T x)
{
  using lib::get_pointer;
  return get_pointer(x) == 0;
}

}  // namespace user

int test()
{
  typedef void * pv;
  int first = user::f(pv());
  int second = user::f(lib::inner::X<int>());
  return first == 1 && second == 1 ? 0 : 1;
}

}  // namespace boost_no_using_breaks_adl

int main()
{
  return boost_no_using_breaks_adl::test();
}
