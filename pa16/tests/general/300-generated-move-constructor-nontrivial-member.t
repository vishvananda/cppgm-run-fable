struct Owner
{
  explicit Owner(int * ptr) : ptr(ptr) {}
  Owner(Owner && other) : ptr(other.ptr) { other.ptr = 0; }
  int * ptr;
};

struct Wrapper
{
  explicit Wrapper(int * ptr) : owner(ptr) {}
  Owner owner;
};

int main()
{
  int value = 7;
  Wrapper source(&value);
  Wrapper moved(static_cast<Wrapper &&>(source));
  return source.owner.ptr == 0 && moved.owner.ptr == &value ? 0 : 1;
}
// VALIDATION: compile-pass
