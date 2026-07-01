namespace mpl {
template<class A, class B, class C, class D, class E>
struct vector {
  static const int value = 5;
};
}

namespace fusion {
template<class A, class B, class C, class D, class E>
struct vector {
  static const int value = 7;
};
}

struct X {};
struct Y {};

int main()
{
  using namespace fusion;
  using mpl::vector;
  typedef vector<Y, char, long, X, bool> mpl_vec;
  return mpl_vec::value == 5 ? 0 : 1;
}
// VALIDATION: compile-pass
