typedef unsigned long size_t;

template<class... T> struct list {
};

template<class L, template<class...> class Y> struct rename_impl;

template<template<class...> class L, class... T, template<class...> class Y>
struct rename_impl<L<T...>, Y> {
  typedef Y<T...> type;
};

template<class L, template<class...> class Y>
using rename = typename rename_impl<L, Y>::type;

template<class L1, class L2> struct assign_impl;

template<template<class...> class L1, class... T,
         template<class...> class L2, class... U>
struct assign_impl<L1<T...>, L2<U...> > {
  typedef L1<U...> type;
};

template<class L1, class L2>
using assign = typename assign_impl<L1, L2>::type;

template<size_t N, class L, class E = void> struct take_c_impl;

template<template<class...> class L, class... T>
struct take_c_impl<0, L<T...> > {
  typedef L<> type;
};

template<template<class...> class L, class T1, class... T>
struct take_c_impl<1, L<T1, T...> > {
  typedef L<T1> type;
};

template<class L, size_t N>
using take_c = assign<L, typename take_c_impl<N, rename<L, list> >::type>;

template<class L, class N>
using take = take_c<L, size_t{N::value}>;

template<class L, class I, class J>
using slice = take<take<L, J>, I>;

template<int N> struct index {
  static const int value = N;
};

typedef slice<list<int, char>, index<1>, index<1> > slice_type;

template<class A, class B> struct same {
  static const bool value = false;
};

template<class A> struct same<A, A> {
  static const bool value = true;
};

static_assert(same<slice_type, list<int> >::value, "");

int main()
{
  return sizeof(slice_type) == 0;
}
