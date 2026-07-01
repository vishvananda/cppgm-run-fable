// VALIDATION: compile-pass
// N3485 focus: 10.2 [class.member.lookup], 12.3.2 [class.conv.fct], 14.7.1 [temp.inst]

struct empty_base
{
};

struct arg1
{
  int marker;

  arg1()
    : marker(7)
  {
  }
};

struct arg2
{
  int marker;

  arg2()
    : marker(11)
  {
  }
};

template<class T, class Base>
struct convertible_to : Base
{
  T value;

  convertible_to()
    : Base(),
      value()
  {
  }

  operator const T &() const
  {
    return value;
  }
};

typedef convertible_to<arg1, empty_base> arg1_conversion_base;
typedef convertible_to<arg2, arg1_conversion_base> source_type;

int take_arg1(const arg1 & value)
{
  return value.marker;
}

int take_arg2(const arg2 & value)
{
  return value.marker;
}

int main()
{
  source_type value;
  return take_arg1(value) == 7 && take_arg2(value) == 11 ? 0 : 1;
}
