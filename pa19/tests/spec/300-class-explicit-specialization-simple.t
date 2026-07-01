// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec]

template<class T>
struct box
{
  int get() const { return 1; }
};

template<>
struct box<int>
{
  int get() const { return 2; }
};

int main()
{
  box<int> b;
  return b.get() == 2 ? 0 : 1;
}
