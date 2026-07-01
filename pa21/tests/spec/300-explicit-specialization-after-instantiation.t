// VALIDATION: compile-fail
// N3485 focus: 14.7.3 [temp.expl.spec]

template<class T>
struct box
{
  static const int value = 1;
};

int already_instantiated = box<int>::value;

template<>
struct box<int>
{
  static const int value = 2;
};

int main()
{
  return already_instantiated;
}
