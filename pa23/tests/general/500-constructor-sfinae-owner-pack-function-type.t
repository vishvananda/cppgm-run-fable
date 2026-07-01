// VALIDATION: compile-pass
// N3485 focus: 14.7.1 [temp.inst], 14.8.2.1 [temp.deduct.call]

template<bool B, class T = void>
struct enable_if
{};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

template<class Base, class Derived>
struct is_base_of
{
  static const bool value = false;
};

struct executor_base
{};

template<class T>
struct is_executor
{
  static const bool value = false;
};

template<int I, class Props>
struct supportable_properties;

template<int I, class Prop>
struct supportable_properties<I, void(Prop)>
{
  template<class T>
  struct is_valid_target
  {
    static const bool value = true;
  };
};

template<int I, class Head, class... Tail>
struct supportable_properties<I, void(Head, Tail...)>
{
  template<class T>
  struct is_valid_target
  {
    static const bool value =
        supportable_properties<I, void(Head)>::
          template is_valid_target<T>::value &&
        supportable_properties<I + 1, void(Tail...)>::
          template is_valid_target<T>::value;
  };
};

template<class T, class Props>
struct is_valid_target_executor
{
  static const bool value =
      is_executor<T>::value &&
      supportable_properties<0, Props>::template is_valid_target<T>::value;
};

struct tracked
{};

struct untracked
{};

template<class T>
struct prefer_only
{};

struct pool
{
  struct executor
  {};

  executor get_executor()
  {
    return executor();
  }
};

template<>
struct is_executor<pool::executor>
{
  static const bool value = true;
};

template<class... SupportableProperties>
struct any_executor : executor_base
{
  template<class Executor>
  any_executor(Executor,
               enable_if_t<
                 !is_same<Executor, any_executor>::value &&
                 !is_base_of<executor_base, Executor>::value &&
                 is_valid_target_executor<
                   Executor,
                   void(SupportableProperties...)>::value>* = 0)
      : selected(1)
  {}

  int selected;
};

int main()
{
  pool p;
  any_executor<prefer_only<tracked>, prefer_only<untracked> > ex(
      p.get_executor());
  return ex.selected == 1 ? 0 : 1;
}
