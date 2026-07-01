// Reduced from Boost.Core type_name. A class partial specialization with a
// template-template parameter must match against defaulted class-template
// arguments, including nested cv-qualified template parameters.

template<class T> struct less {};
template<class T> struct hash {};
template<class T> struct equal_to {};
template<class T> struct allocator {};
template<class T, class U> struct pair {};

template<class K, class V, class C = less<K>,
         class A = allocator<pair<K const, V> > >
struct map {};

template<class K, class V, class H = hash<K>, class Eq = equal_to<K>,
         class A = allocator<pair<K const, V> > >
struct unordered_map {};

struct value {};

template<class T>
struct holder {
  static const int value = 0;
};

template<template<class...> class L, class... T>
struct holder< L<T...> > {
  static const int value = 1;
};

template<template<class T, class U, class Pr, class A> class L, class T, class U>
struct holder< L<T, U, less<T>, allocator<pair<T const, U> > > > {
  static const int value = 2;
};

template<template<class T, class U, class H, class Eq, class A> class L, class T, class U>
struct holder< L<T, U, hash<T>, equal_to<T>, allocator<pair<T const, U> > > > {
  static const int value = 3;
};

static_assert(holder<map<int, value> >::value == 2, "map partial");
static_assert(holder<unordered_map<int, value> >::value == 3, "unordered_map partial");

int main()
{
  return holder<map<int, value> >::value * 10 +
         holder<unordered_map<int, value> >::value == 23 ? 0 : 1;
}
