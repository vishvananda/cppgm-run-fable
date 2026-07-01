// VALIDATION: compile-pass
// N3485 focus: 14.5.4 [temp.friend]

template<typename T>
struct Box
{
  Box(T value) : value(value) {}

private:
  T value;

  template<typename U>
  friend U read(Box<U>);
};

template<typename U>
U read(Box<U> box)
{
  return box.value;
}

int main()
{
  Box<int> box(11);
  return read(box) == 11 ? 0 : 1;
}
