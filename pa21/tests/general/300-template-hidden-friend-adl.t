// VALIDATION: compile-pass
// N3485 focus: 3.4.2 [basic.lookup.argdep], 14.6.4 [temp.dep.res]

template<typename T>
struct Box
{
  T value;

  friend T get(Box b)
  {
    return b.value;
  }
};

int main()
{
  Box<int> b = {9};
  return get(b) == 9 ? 0 : 1;
}
