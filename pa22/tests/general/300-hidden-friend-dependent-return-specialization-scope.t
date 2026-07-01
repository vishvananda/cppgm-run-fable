// VALIDATION: compile-pass
// A hidden friend with a dependent enable-if return type must be found by ADL
// while evaluating a detector on a class-template specialization.

template <bool B, typename T = void>
struct enable_if
{
};

template <typename T>
struct enable_if<true, T>
{
  typedef T type;
};

template <bool B, typename T = void>
using enable_if_t = typename enable_if<B, T>::type;

template <typename T>
T&& declval();

template <typename...>
using void_t = void;

struct true_type
{
  static constexpr bool value = true;
};

struct false_type
{
  static constexpr bool value = false;
};

template <typename Base>
true_type base_test(const Base*);

false_type base_test(...);

template <typename Base, typename Derived>
struct is_base_of
{
  static constexpr bool value =
      decltype(base_test<Base>(static_cast<Derived*>(0)))::value;
};

template <typename T, typename = void>
struct has_equal
{
  static constexpr bool value = false;
};

template <typename T>
struct has_equal<T, void_t<decltype(declval<const T>() == declval<const T>())> >
{
  static constexpr bool value = true;
};

template <typename... SupportableProperties>
struct any_executor
{
  template <typename AnyExecutor1, typename AnyExecutor2>
  friend enable_if_t<
      is_base_of<any_executor, AnyExecutor1>::value ||
          is_base_of<any_executor, AnyExecutor2>::value,
      bool>
  operator==(const AnyExecutor1&, const AnyExecutor2&)
  {
    return true;
  }
};

struct io_property
{
};

struct completion_property
{
};

struct any_io_executor : any_executor<io_property>
{
};

struct any_completion_executor : any_executor<completion_property>
{
  template <typename Executor,
            typename = enable_if_t<has_equal<Executor>::value> >
  any_completion_executor(Executor)
  {
  }
};

static_assert(has_equal<any_completion_executor>::value,
              "completion specialization has its own hidden friend");

int main()
{
  any_io_executor ex;
  any_completion_executor converted(ex);
  return has_equal<any_completion_executor>::value ? 0 : 1;
}
