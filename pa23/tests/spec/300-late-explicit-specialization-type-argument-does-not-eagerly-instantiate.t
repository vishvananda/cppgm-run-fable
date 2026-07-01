// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec], 14.7.1 [temp.inst]

namespace n {

template<class Tag, class Op, class T, class U>
struct flag
{
  static const bool value = false;
};

template<class T = void>
struct plus
{
  T operator()(const T& x, const T& y) const { return x + y; }
};

template<class T, class U>
struct flag<int, plus<void>, T, U>
{
  static const bool value = true;
};

template<>
struct plus<void>
{
  template<class T1, class T2>
  auto operator()(T1&& t, T2&& u) const -> decltype(t + u)
  {
    return t + u;
  }
};

}  // namespace n

int main()
{
  return n::flag<int, n::plus<void>, int, int>::value ? 0 : 1;
}
