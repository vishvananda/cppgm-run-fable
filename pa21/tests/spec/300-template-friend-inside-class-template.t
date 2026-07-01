// VALIDATION: compile-pass
// N3485 focus: 14.5.4 [temp.friend], 14.6.5 [temp.inject]

template<typename T>
struct box
{
private:
  int value;

public:
  explicit box(int v) : value(v) {}

  template<typename U>
  friend int read(box<U> b);
};

template<typename U>
int read(box<U> b)
{
  return b.value;
}

int main()
{
  return read(box<int>(9)) == 9 ? 0 : 1;
}
