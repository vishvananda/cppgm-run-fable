// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]; variable templates are post-N3485.

template<class T, class U>
constexpr int pick_v = 0;

template<class T>
constexpr int pick_v<T, T *> = 1;

int main()
{
  return pick_v<int, int *> == 1 ? 0 : 1;
}
