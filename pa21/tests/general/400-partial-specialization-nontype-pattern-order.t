// VALIDATION: compile-pass

namespace detail
{
struct unspecified
{
};
}

template<int I, class Ret = detail::unspecified>
struct function_action
{
};

template<class Act, class Args>
struct return_type_N;

template<int I, class Args, class Ret>
struct return_type_N<function_action<I, Ret>, Args>
{
  typedef Ret type;
};

template<int I, class Args>
struct return_type_N<function_action<I, detail::unspecified>, Args>
{
  typedef int type;
};

typedef return_type_N<function_action<3, detail::unspecified>, void>::type selected;

int main()
{
  selected value = 0;
  return value;
}
