// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call]

namespace array_bound_only_deduction
{
typedef unsigned long size_t;

struct tag
{
  template<size_t S>
  friend char *to_zstr(char (&zstr)[S], tag const &)
  {
    return zstr;
  }
};
}

int main()
{
  char buf[5];
  return to_zstr(buf, array_bound_only_deduction::tag()) == buf ? 0 : 1;
}
