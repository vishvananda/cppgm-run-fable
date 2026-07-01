// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

static_assert(!is_same<char *&, char *const &>::value, "");
static_assert(!is_same<const char *&, const char *const &>::value, "");

int main()
{
  return 0;
}
