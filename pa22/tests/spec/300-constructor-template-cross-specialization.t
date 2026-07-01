// VALIDATION: compile-pass
// N3485 focus: 14.8 [temp.fct.spec]

template<typename T>
struct wrap
{
  int value;

  explicit wrap(int v) : value(v) {}

  template<typename U>
  wrap(const wrap<U> & other) : value(other.value + 10) {}
};

int main()
{
  wrap<int> src(3);
  wrap<long> dst(src);
  return dst.value == 13 ? 0 : 1;
}
