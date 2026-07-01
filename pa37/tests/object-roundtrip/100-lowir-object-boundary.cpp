template<class T>
struct Box {
  static T add(T lhs, T rhs)
  {
    return lhs + rhs;
  }
};

static int seed = 5;

int exported_add(int value)
{
  return Box<int>::add(value, seed);
}

extern "C" int c_entry(int value)
{
  return exported_add(value) + 1;
}
