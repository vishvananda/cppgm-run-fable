// VALIDATION: compile-pass
// Reduced from Boost.Core type_name. A class partial specialization pattern
// `T*` must not match a top-level cv-qualified pointer; the cv-wrapper partial
// owns that match, even when reached through a recursive pointer formatter.

template<class T>
struct holder {
  static const int value = 0;
  static int render() { return 0; }
};

template<class T>
struct holder<T const> {
  static const int value = 1;
  static int render() { return holder<T>::render() + 1; }
};

template<class T>
struct holder<T volatile> {
  static const int value = 2;
  static int render() { return holder<T>::render() + 2; }
};

template<class T>
struct holder<T*> {
  static const int value = 4;
  static int render() { return holder<T>::render() + 4; }
};

class B;

static_assert(holder<B const* volatile>::value == 2, "top cv pointer");
static_assert(holder<B const* volatile*>::value == 4, "outer pointer");

int main()
{
  return holder<B const* volatile*>::render() == 11 ? 0 : 1;
}
