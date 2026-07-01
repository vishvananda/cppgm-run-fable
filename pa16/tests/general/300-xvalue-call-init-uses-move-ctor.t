struct Owner
{
  explicit Owner(int * ptr) : ptr(ptr) {}
  Owner(Owner && other) : ptr(other.ptr) { other.ptr = 0; }
  Owner(const Owner &) = delete;
  int * ptr;
};

Owner && as_rvalue(Owner & owner)
{
  return static_cast<Owner &&>(owner);
}

Owner make(Owner & owner)
{
  return as_rvalue(owner);
}

int main()
{
  int value = 7;
  Owner source(&value);
  Owner moved = make(source);
  return source.ptr == 0 && moved.ptr == &value ? 0 : 1;
}
// VALIDATION: compile-pass
