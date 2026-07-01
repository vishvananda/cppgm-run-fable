// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.3.2 [temp.arg.nontype]

template<class T, int N = sizeof(T)>
struct sized
{
  static const int value = N;
};

int main()
{
  return sized<int>::value == sizeof(int) ? 0 : 1;
}
