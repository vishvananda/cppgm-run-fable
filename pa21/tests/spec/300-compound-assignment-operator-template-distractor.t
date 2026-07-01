// VALIDATION: compile-pass
// N3485 focus: 13.5 [over.oper], 14.8.2 [temp.deduct]

struct Property
{
  typedef unsigned long const& read_access_t;

  unsigned long value;

  Property() : value(0) {}

  operator read_access_t() const
  {
    return value;
  }
};

struct Holder
{
  Property p;

  void operator+=(Holder const& rhs);

  template<class T>
  void operator+=(T const& rhs);
};

template<class T>
struct Text
{
  T value;
};

template<class T>
Text<T>& operator+=(Text<T>& target, Property const& rhs);

void Holder::operator+=(Holder const& rhs)
{
  p.value += rhs.p;
}

int main()
{
  Holder lhs;
  Holder rhs;
  lhs.p.value = 2;
  rhs.p.value = 5;
  lhs += rhs;
  return lhs.p.value == 7 ? 0 : 1;
}
