struct piecewise_construct_t {};

template<class... T>
struct tuple {};

template<template<class...> class Tuple, class... Args1, class... Args2>
int dispatch(piecewise_construct_t, Tuple<Args1...>, Tuple<Args2...>)
{
  return 0;
}

int main()
{
  return dispatch(piecewise_construct_t(), tuple<>(), tuple<>());
}
