template<class T, T V>
struct integral_constant
{
  static const T value = V;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<bool B, class T, class U>
struct conditional
{
  typedef T type;
};

template<class T, class U>
struct conditional<false, T, U>
{
  typedef U type;
};

template<class A, class B>
struct is_same : false_type
{};

template<class A>
struct is_same<A, A> : true_type
{};

struct any_executor_base
{};

template<class Base, class Derived>
struct is_base_of : false_type
{};

template<bool B>
struct constraint
{};

template<>
struct constraint<true>
{
  typedef int type;
};

template<class T>
struct is_executor : true_type
{};

struct prop_a
{};

struct prop_b
{};

template<int I, class Props>
struct supportable_properties;

template<int I, class Prop>
struct supportable_properties<I, void(Prop)>
{
  template<class T>
  struct is_valid_target : true_type
  {};
};

template<int I, class Head, class... Tail>
struct supportable_properties<I, void(Head, Tail...)>
{
  template<class T>
  struct is_valid_target : integral_constant<bool,
      supportable_properties<I, void(Head)>::
        template is_valid_target<T>::value &&
      supportable_properties<I + 1, void(Tail...)>::
        template is_valid_target<T>::value>
  {};
};

template<class T, class Props>
struct is_valid_target_executor :
    conditional<
      is_executor<T>::value,
      typename supportable_properties<0, Props>::template is_valid_target<T>,
      false_type
    >::type
{};

struct executor
{};

struct any_completion_executor
{
  typedef void supportable_properties_type(prop_a, prop_b);

  template<class Executor>
  any_completion_executor(Executor,
      typename constraint<
        conditional<
          !is_same<Executor, any_completion_executor>::value &&
            !is_base_of<any_executor_base, Executor>::value,
          is_valid_target_executor<Executor, supportable_properties_type>,
          false_type
        >::type::value
      >::type = 0)
      : selected(1)
  {}

  int selected;
};

int main()
{
  executor ex;
  any_completion_executor value(ex);
  return value.selected == 1 ? 0 : 1;
}
