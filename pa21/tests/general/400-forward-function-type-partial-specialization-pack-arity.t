// VALIDATION: compile-pass

template<class>
struct box;

template<class R, class... Args>
struct box<R(Args...)>
{
  static const int arity = sizeof...(Args);
};

int main()
{
  return box<int()>::arity != 0 || box<int(int, float)>::arity != 2;
}
