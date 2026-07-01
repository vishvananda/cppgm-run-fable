// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.8 [temp.fct.spec]

struct probe
{
  template<class T>
  int tag(const T &);

  template<class T>
  int tag(const T &) const
  {
    return 2;
  }

  template<class T>
  int find(const T & value) const
  {
    return tag(value);
  }
};

template<class T>
int probe::tag(const T &)
{
  return 1;
}

int main()
{
  const probe p = probe();
  return p.find(0) == 2 ? 0 : 1;
}
