template <class T, class U, class = void>
const bool same_type_value = false;

template <class T>
const bool same_type_value<T, T, void> = true;

template <class T, T value>
struct constant
{
  static const T result = value;
};

struct S
{
};

template <class T, class U>
using wrapped_same = constant<bool, same_type_value<T, U> >;

static_assert(wrapped_same<S, S>::result,
              "alias target should keep substituted variable-template arguments");

int main()
{
  return wrapped_same<S, S>::result ? 0 : 1;
}
