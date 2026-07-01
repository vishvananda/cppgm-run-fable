// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.8.1 [temp.arg.explicit]
// A member-template body must resolve explicit template arguments in the
// member-template parameter scope, even when a dependent base used the same
// parameter names.

namespace ns {
template <class T> struct remove_reference { typedef T type; };
template <class T> struct remove_reference<T &> { typedef T type; };
template <class T> struct remove_reference<T &&> { typedef T type; };

template <class T>
T && forward(typename remove_reference<T>::type & t)
{
  return static_cast<T &&>(t);
}

template <class T>
T && forward(typename remove_reference<T>::type && t)
{
  return static_cast<T &&>(t);
}

template <class U1, class U2>
struct Base {};

template <class T1, class T2>
struct Pair : public Base<T1, T2> {
  T1 first;
  T2 second;

  template <class U1, class U2>
  Pair(U1 && x, U2 && y)
      : first(ns::forward<U1>(x)), second(ns::forward<U2>(y))
  {
  }
};
}

int main()
{
  ns::Pair<unsigned long, unsigned long> p(1u, 2u);
  return p.first == 1 && p.second == 2 ? 0 : 1;
}
