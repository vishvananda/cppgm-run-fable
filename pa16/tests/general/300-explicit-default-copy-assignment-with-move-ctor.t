struct Item
{
  Item() : value(0) {}
  Item(Item && other) : value(other.value) { other.value = 0; }
  Item & operator=(const Item &) = default;
  int value;
};

int main()
{
  Item a;
  a.value = 7;
  const Item b;
  a = b;
  return a.value;
}
// VALIDATION: compile-pass
