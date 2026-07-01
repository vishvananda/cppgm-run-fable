// VALIDATION: compile-pass

namespace lib {
struct String
{
  int value;

  String() : value(0)
  {
  }

  String & operator+=(char const *)
  {
    value = 100;
    return *this;
  }
};
}

namespace boost {
struct Cstring
{
};

lib::String & operator+=(lib::String & target, Cstring const &)
{
  target.value = 7;
  return target;
}
}

struct Holder
{
  lib::String value;
};

int main()
{
  Holder h;
  boost::Cstring s;
  h.value += s;
  return h.value.value == 7 ? 0 : 1;
}
