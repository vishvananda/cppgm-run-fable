// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]; variable templates are post-N3485.

template<class Tag, class Op, class... Args>
inline const bool v = false;

struct eq
{
  template<class A, class B>
  bool operator()(const A &, const B &) const
  {
    return true;
  }
};

template<class A, class B>
inline const bool v<int, eq, A, B> = true;

int main()
{
  return v<int, eq, int, int> ? 0 : 1;
}
