// N3485 focus: [temp.arg.explicit], [temp.deduct.call].
// Explicit member function-template arguments must drop overloads whose
// parameter kind does not match the carried argument syntax.

struct foo {
  template<class T>
  int bar() { return 0; }

  template<int I>
  int bar() { return 1; }
};

int main()
{
  foo f;
  int a = f.bar<char>();
  int b = f.bar<2>();
  return a != 0 || b != 1;
}
