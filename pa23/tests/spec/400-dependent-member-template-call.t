// VALIDATION: compile-pass
// N3485 focus: 14.2 [temp.names], 14.6.4 [temp.dep.res]

template<typename Tag>
struct wrapper
{
  template<typename T>
  T cast(T value)
  {
    return value;
  }
};

template<typename Tag>
int run(wrapper<Tag> & w)
{
  return w.template cast<int>(3);
}

int main()
{
  wrapper<int> w;
  return run(w) == 3 ? 0 : 1;
}
