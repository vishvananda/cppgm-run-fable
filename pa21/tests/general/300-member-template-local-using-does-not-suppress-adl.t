// VALIDATION: compile-pass
// libc++ error_condition reduction: a local using-declaration inside a member
// template should not be mistaken for class-member lookup that suppresses ADL.

namespace lib
{
enum class errc
{
  value
};

namespace adl_only
{
void make_error_condition() = delete;
}

struct condition
{
  int n;

  condition() : n(0) {}
  condition(int value) : n(value) {}

  condition& operator=(const condition& other)
  {
    n = other.n;
    return *this;
  }

  template<class E>
  condition(E e)
  {
    using adl_only::make_error_condition;
    *this = make_error_condition(e);
  }
};

inline condition make_error_condition(errc)
{
  return condition(7);
}
}

int main()
{
  lib::condition c(lib::errc::value);
  return c.n == 7 ? 0 : 1;
}
