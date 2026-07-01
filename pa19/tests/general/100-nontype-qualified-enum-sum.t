// VALIDATION: compile-pass
// A parenthesized qualified enum member in an integral non-type argument can
// look like a C-style cast while parsing; semantic evaluation must recover it
// as a value expression before combining the sum.

template<class T>
struct holder
{
  enum { value = 0 };
};

template<class T>
struct A
{
};

template<class T>
struct B
{
};

template<int>
struct box
{
};

box<(0 + (holder<A<int> >::value) + (holder<B<int> >::value))> value;

int main()
{
  (void)value;
  return 0;
}
