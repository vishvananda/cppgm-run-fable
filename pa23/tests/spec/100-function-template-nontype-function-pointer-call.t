// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype], 14.8.1 [temp.arg.explicit]

namespace api
{
namespace detail
{
template<void (*Test)()>
void run_test(const char *)
{
  Test();
}
}
}

int calls = 0;

void sample()
{
  calls = 1;
}

int main()
{
  api::detail::run_test<&sample>("sample");
  return calls == 1 ? 0 : 1;
}
