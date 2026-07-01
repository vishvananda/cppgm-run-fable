// VALIDATION: compile-pass
// HHC-102

typedef unsigned long uintptr_t;

struct Buffer
{
  void *current;

  uintptr_t address() const
  {
    return uintptr_t(current);
  }
};

int main()
{
  int value = 0;
  Buffer buffer;
  buffer.current = &value;
  return buffer.address() == uintptr_t(&value) ? 0 : 1;
}
