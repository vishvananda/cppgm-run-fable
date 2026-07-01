// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype], 7 [dcl.dcl] static_assert

constexpr int K = 3;

template<int N>
struct box
{
  static_assert(N > 0, "ok");
  int a[N];

  int get() { return sizeof(a); }
};

int main()
{
  box<K + 1> b;
  return b.get() == 4 * (K + 1) ? 0 : 1;
}
