#include <type_traits>
namespace boost
{
namespace asio
{
using std::conditional;
using std::false_type;
using std::is_base_of;
using std::is_same;
using std::true_type;
template<bool B, class T = int> struct constraint {};
template<class T> struct constraint<true, T> { typedef T type; };
template<bool B, class T = int> using constraint_t = typename constraint<B, T>::type;
namespace execution { namespace detail {
struct any_executor_base {};
template<class Executor, class Props> struct is_valid_target_executor : true_type {};
} }
class any_completion_executor : public execution::detail::any_executor_base
{
public:
  typedef void supportable_properties_type();
  template<class Executor>
  any_completion_executor(Executor,
      constraint_t<
        conditional<
          !is_same<Executor, any_completion_executor>::value &&
            !is_base_of<execution::detail::any_executor_base, Executor>::value,
          execution::detail::is_valid_target_executor<Executor, supportable_properties_type>,
          false_type
        >::type::value
      > = 0)
  {}
};
}
}
struct fat_executor {};
static_assert(std::is_constructible<boost::asio::any_completion_executor, fat_executor>::value,
              "SFINAE constructor accepts fat_executor");
void imported_base_constructor_anchor()
{
  boost::asio::any_completion_executor ex = fat_executor();
  (void)ex;
}
static_assert(sizeof(&imported_base_constructor_anchor) > 0, "SFINAE constructor body anchor");
