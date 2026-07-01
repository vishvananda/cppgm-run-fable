// VALIDATION: compile-pass
// N3485 focus: 14.5.4 [temp.friend], 14.6.4 [temp.dep.res]

struct sink
{
  int value;
};

struct manip
{
  int delta;

  template<class T>
  friend T & operator<<(T & out, const manip & m)
  {
    out.value = out.value + m.delta;
    return out;
  }
};

int main()
{
  sink s;
  manip a;
  manip b;
  s.value = 1;
  a.delta = 2;
  b.delta = 3;
  sink * p = &(s << a << b);
  return p == &s && s.value == 6 ? 0 : 1;
}
