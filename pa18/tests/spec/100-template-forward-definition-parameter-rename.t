// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.5.1 [temp.class]

template<class T>
struct box;

typedef box<int> int_box;

template<class U>
struct box
{
  U value;
  U get() { return value; }
};

int main()
{
  int_box x;
  x.value = 3;
  return x.get() == 3 ? 0 : 1;
}
