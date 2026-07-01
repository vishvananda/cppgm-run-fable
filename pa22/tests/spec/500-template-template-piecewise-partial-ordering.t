// VALIDATION: compile-pass
// N3485 focus: 14.8.2.4 [temp.deduct.partial], 14.8.2.5 [temp.deduct.type]
// Boost.Container scoped_allocator reduction: fixed template-template tuple
// overloads must order against both more-null fixed arities and a broader
// variadic template-template fallback.

struct fixed_null_type {};
struct fixed_piecewise_t {};

template<class A, class B>
struct fixed_pair {};

template<class T0 = fixed_null_type,
         class T1 = fixed_null_type,
         class T2 = fixed_null_type>
struct fixed_tuple {};

struct fixed_alloc {};
struct fixed_scoped {};

int fixed_selected = 0;

template<class ConstructAlloc, class AllocArg, class Pair,
         template<class, class, class> class BoostTuple,
         class X0, class Y0>
int fixed_dispatch(ConstructAlloc &, AllocArg &, Pair *, fixed_piecewise_t,
                   BoostTuple<X0, fixed_null_type, fixed_null_type>,
                   BoostTuple<Y0, fixed_null_type, fixed_null_type>)
{
  fixed_selected = 1;
  return fixed_selected;
}

template<class ConstructAlloc, class AllocArg, class Pair,
         template<class, class, class> class BoostTuple,
         class X0, class X1, class Y0, class Y1>
int fixed_dispatch(ConstructAlloc &, AllocArg &, Pair *, fixed_piecewise_t,
                   BoostTuple<X0, X1, fixed_null_type>,
                   BoostTuple<Y0, Y1, fixed_null_type>)
{
  fixed_selected = 2;
  return fixed_selected;
}

int run_fixed_arity()
{
  fixed_alloc a;
  fixed_scoped s;
  fixed_pair<int, int> p;
  return fixed_dispatch(a, s, &p, fixed_piecewise_t(),
                        fixed_tuple<int, fixed_null_type, fixed_null_type>(),
                        fixed_tuple<int, fixed_null_type, fixed_null_type>()) == 1 ? 0 : 1;
}

struct variadic_null_type {};
struct variadic_piecewise_t {};

template<class A, class B>
struct variadic_pair {};

template<class T0 = variadic_null_type,
         class T1 = variadic_null_type,
         class T2 = variadic_null_type>
struct variadic_tuple {};

struct variadic_alloc {};
struct variadic_scoped {};

int variadic_selected = 0;

template<class ConstructAlloc, class AllocArg, class Pair,
         template<class, class, class> class BoostTuple,
         class X0, class Y0>
int variadic_dispatch(ConstructAlloc &, AllocArg &, Pair *, variadic_piecewise_t,
                      BoostTuple<X0, variadic_null_type, variadic_null_type>,
                      BoostTuple<Y0, variadic_null_type, variadic_null_type>)
{
  variadic_selected = 1;
  return variadic_selected;
}

template<class ConstructAlloc, class AllocArg, class Pair,
         template<class...> class Tuple, class... Args1, class... Args2>
int variadic_dispatch(ConstructAlloc &, AllocArg &, Pair *, variadic_piecewise_t,
                      Tuple<Args1...>, Tuple<Args2...>)
{
  variadic_selected = 2;
  return variadic_selected;
}

int run_variadic_fallback()
{
  variadic_alloc a;
  variadic_scoped s;
  variadic_pair<int, int> p;
  return variadic_dispatch(
             a, s, &p, variadic_piecewise_t(),
             variadic_tuple<int, variadic_null_type, variadic_null_type>(),
             variadic_tuple<int, variadic_null_type, variadic_null_type>()) == 1 ? 0 : 2;
}

int main()
{
  int a = run_fixed_arity();
  if(a != 0) {
    return a;
  }
  return run_variadic_fallback();
}
