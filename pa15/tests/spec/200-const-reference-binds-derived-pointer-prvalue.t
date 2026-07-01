// VALIDATION: compile-pass
// N3485 focus: 8.5.3 [dcl.init.ref], 4.10 [conv.ptr]

struct Base
{
  int b;
};

struct Derived : Base
{
  int d;
};

Derived object;
Base * saved;

Derived & get()
{
  return object;
}

void push(Base * const & p)
{
  saved = p;
}

int main()
{
  push(&get());
  return saved == &object ? 0 : 1;
}
