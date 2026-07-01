// VALIDATION: compile-fail
// N3485 focus: 4.10 [conv.ptr], 10 [class.derived], 13.3.1 [over.match.funcs]

struct Cell
{
  int value;
};

struct Iter
{
  Cell * operator->() const;
};

struct ConstIter
{
  const Cell * operator->() const;
};

struct Base
{
  Iter begin();
  ConstIter begin() const;
};

struct Derived : Base
{
};

int main()
{
  const Derived d;
  d.begin()->value = 1;
  return 0;
}
