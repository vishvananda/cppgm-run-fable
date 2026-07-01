// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec]

template<class T>
struct tag
{
  static int id()
  {
    return 1;
  }
};

template<>
int tag<int>::id()
{
  return 2;
}

int main()
{
  return tag<char>::id() == 1 && tag<int>::id() == 2 ? 0 : 1;
}
