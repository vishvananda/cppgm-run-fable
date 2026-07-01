// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]

template<class T, class U, class = void>
struct same
{
  static const bool value = false;
};

template<class T>
struct same<T, T>
{
  static const bool value = true;
};

static_assert(same<char, char>::value, "");

int main()
{
  return same<int, char>::value ? 1 : 0;
}
