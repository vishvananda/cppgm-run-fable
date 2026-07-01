template<typename T, T value>
struct integral_constant
{
  static constexpr T value_value = value;
};

template<bool value>
using bool_constant = integral_constant<bool, value>;

template<class T, class... Args>
struct is_constructible : bool_constant<__is_constructible(T, Args...)>
{
};

struct Target
{
};

template<typename T>
struct DirectOnly
{
  explicit DirectOnly(T *)
  {
  }
};

struct Other
{
};

static_assert(is_constructible<DirectOnly<Target>, Target *>::value_value, "");
static_assert(!is_constructible<DirectOnly<Target>, Other *>::value_value, "");

int main()
{
  return 0;
}
