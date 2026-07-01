// VALIDATION: compile-pass
// Direct-object ABI parameters are already initialized at the call boundary.
// Materializing the local parameter slot must copy the object payload, not pass
// the payload itself as the address of a constructor source object.

struct Base
{
  Base(void * first, void * second) : first(first), second(second) {}
  Base(const Base &) = default;

  void * first;
  void * second;
};

struct Pair : Base
{
  Pair(void * first, void * second) : Base(first, second) {}
  Pair(const Pair & other) : Base(other) {}
};

int seen = 0;

void use(const Pair & value)
{
  if(value.first == 0 && value.second == reinterpret_cast<void *>(16)) {
    seen = 1;
  }
}

void check(Pair value)
{
  Pair copy(value);
  use(copy);
}

int main()
{
  Pair input(0, reinterpret_cast<void *>(16));
  check(input);
  return seen == 1 ? 0 : 1;
}
