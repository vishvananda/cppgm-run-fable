// VALIDATION: compile-pass
// N3485 focus: 14.7.1 [temp.inst], 7.1.3 [dcl.typedef]

template<class T>
struct lazy_box {
  typename T::missing_type *p;
};

typedef lazy_box<int> lazy_alias;

int main()
{
  return 0;
}
