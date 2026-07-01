// VALIDATION: compile-pass

template<int N>
struct tag
{
  static const int value = N;
};

template<class T>
struct cv_traits
{
  typedef tag<0> tag_type;
};

template<class T>
struct cv_traits<T const &>
{
  typedef tag<1> tag_type;
};

template<class T>
struct cv_traits<T volatile &>
{
  typedef tag<2> tag_type;
};

template<class T>
struct cv_traits<T const volatile &>
{
  typedef tag<3> tag_type;
};

class C;

static_assert(cv_traits<C &>::tag_type::value == 0, "");
static_assert(cv_traits<C const &>::tag_type::value == 1, "");
static_assert(cv_traits<C volatile &>::tag_type::value == 2, "");
static_assert(cv_traits<C const volatile &>::tag_type::value == 3, "");

int main()
{
  return cv_traits<C const volatile &>::tag_type::value == 3 ? 0 : 1;
}
