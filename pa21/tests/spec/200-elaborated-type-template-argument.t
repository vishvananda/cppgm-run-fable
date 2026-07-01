// VALIDATION: compile-pass
// N3485 focus: 14.3.1 [temp.arg.type]

template<bool B, class T, class F>
struct IfC
{
  typedef T type;
};

template<class T, class F>
struct IfC<false, T, F>
{
  typedef F type;
};

template<bool B, class T, class F>
using If = typename IfC<B, T, F>::type;

If<false, struct PrivateNat, int> value;

int main()
{
  return value;
}
