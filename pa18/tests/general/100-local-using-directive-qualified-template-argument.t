// VALIDATION: compile-pass

namespace policy_impl
{
namespace sign
{
struct return_sign
{
};
}
}

template<class T>
struct holder
{
  T value;
};

int main()
{
  using namespace policy_impl;
  using X = holder<sign::return_sign>;
  X x;
  return sizeof(x.value) == 1 ? 0 : 1;
}
