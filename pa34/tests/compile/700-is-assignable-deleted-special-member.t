struct DeletedBase
{
  DeletedBase & operator=(const DeletedBase &) = delete;
};

struct Derived : DeletedBase
{
};

struct ConstMember
{
  const int value;
};

static_assert(!__is_assignable(Derived &, const Derived &),
              "deleted base assignment is not assignable");
static_assert(!__is_assignable(ConstMember &, const ConstMember &),
              "const data member assignment is not assignable");
static_assert(__is_assignable(int &, int),
              "scalar assignment remains assignable");

int main()
{
  return 0;
}
// VALIDATION: compile-pass
