// VALIDATION: compile-pass
// A class partial-specialization enable_if condition must re-evaluate a
// substituted inherited bool member value instead of keeping the original
// placeholder-bearing condition dependent.

template<bool Value>
struct bool_constant {
  static constexpr bool value = Value;
};

typedef bool_constant<true> true_type;
typedef bool_constant<false> false_type;

template<bool Cond, class T = void>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool Cond, class T = void>
using enable_if_t = typename enable_if<Cond, T>::type;

template<class T>
struct add_const {
  typedef const T type;
};

template<class T>
using add_const_t = typename add_const<T>::type;

struct executor {
};

struct function_object {
};

template<class T, class F, class = void>
struct execute_member_trait : false_type {
};

template<class F>
struct execute_member_trait<const executor, F, void> : true_type {
};

template<class T, class F, class = void>
struct execute_member_default : execute_member_trait<T, F> {
};

template<class T, class F, class = void>
struct execute_member : execute_member_default<T, F> {
};

template<class T, class F, class = void>
struct is_executor_of_impl : false_type {
};

template<class T, class F>
struct is_executor_of_impl<
    T,
    F,
    enable_if_t<execute_member<add_const_t<T>, F>::value> > : true_type {
};

static_assert(execute_member<add_const_t<executor>, function_object>::value,
              "inner trait value should be inherited");
static_assert(is_executor_of_impl<executor, function_object>::value,
              "outer partial specialization should match");

int main()
{
  return is_executor_of_impl<executor, function_object>::value ? 0 : 1;
}
