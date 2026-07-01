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

template<class T, int N, bool Enabled>
void sample()
{
  calls = sizeof(T) + N + (Enabled ? 1 : 0);
}

int main()
{
  api::detail::run_test<&sample<char, 2, true> >("sample");
  return calls == 4 ? 0 : 1;
}
