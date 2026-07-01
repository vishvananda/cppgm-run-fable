// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.8.2 [temp.deduct]

namespace std {
inline namespace __1 {
template<class T>
T && __declval(int);

template<class T>
struct holder
{
  typedef decltype(std::__declval<T>(0)) type;
};
}
}

std::holder<int>::type f()
{
  return 0;
}

int main()
{
  return f();
}
