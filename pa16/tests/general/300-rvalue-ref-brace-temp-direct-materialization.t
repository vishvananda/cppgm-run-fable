// VALIDATION: compile-pass

struct Base
{
  int value;

  Base() : value(7) {}
  Base(Base const&) : value(-100) {}
  Base(Base&& other) : value(other.value) { other.value = 0; }
  Base& operator=(Base&& other) { value = other.value; other.value = 0; return *this; }
  ~Base() {}
};

struct Box : Base
{
  Box() {}
  Box(Box const&) = default;
  Box(Box&&) = default;
  Box& operator=(Box&&) = default;
  ~Box() {}
};

int assign_from_empty_braces()
{
  Box box;
  box = {};
  return box.value;
}

int main()
{
  return assign_from_empty_braces() == 7 ? 0 : 1;
}
