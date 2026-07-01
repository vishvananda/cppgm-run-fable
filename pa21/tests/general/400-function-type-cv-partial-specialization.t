// VALIDATION: compile-pass
// Function type cv-qualifiers participate in partial specialization matching.

template<class T>
struct holder
{
  static const int value = 0;
};

template<class R, class... A>
struct holder<R(A...)>
{
  static const int value = 1;
};

template<class R, class... A>
struct holder<R(A...) const>
{
  static const int value = 2;
};

template<class R, class... A>
struct holder<R(A...) volatile>
{
  static const int value = 3;
};

static_assert(holder<void()>::value == 1, "");
static_assert(holder<void() const>::value == 2, "");
static_assert(holder<void() volatile>::value == 3, "");

int main()
{
  return holder<void()>::value == 1 ? 0 : 1;
}
