// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]

template<class Sig>
struct arity
{
  static const int value = 0;
};

template<class R, class A>
struct arity<R(A)>
{
  static const int value = 1;
};

template<class R, class A, class... Rest>
struct arity<R(A, Rest...)>
{
  static const int value = 2;
};

template<class Sig>
struct relay;

template<class R, class... Args>
struct relay<R(Args...)> : arity<R(Args...)>
{
};

static_assert(relay<void(int)>::value == 1, "");
static_assert(relay<void(int, float)>::value == 2, "");

int main()
{
  return relay<void(int, float)>::value == 2 ? 0 : 1;
}
