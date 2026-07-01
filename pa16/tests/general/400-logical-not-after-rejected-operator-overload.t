namespace outer
{
namespace detail
{
enum class tag
{
  value = 0
};

bool operator!(tag)
{
  return false;
}
}

struct status
{
  explicit operator bool() const
  {
    return value != 0;
  }

  int value;
};
}

namespace outer
{
namespace detail
{
int run(status s)
{
  return !s ? 1 : 0;
}
}
}

int main()
{
  outer::status s = {0};
  return outer::detail::run(s) == 1 ? 0 : 1;
}
// VALIDATION: compile-pass
