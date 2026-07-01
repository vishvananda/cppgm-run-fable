// VALIDATION: compile-pass

template<class>
struct box;

template<class R, class... Args>
struct box<R(Args...)>
{
  static const int value = 1;
};

template<class... Args>
struct box<void(Args...)>
{
  static const int value = 2;
};

int main()
{
  return box<void(int)>::value == 2 ? 0 : 1;
}
