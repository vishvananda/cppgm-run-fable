// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], expression SFINAE for a data-member
// probe in the non-result operand of a comma expression.

template<typename T>
auto has_value(const T & value, int) -> decltype(value.member, bool())
{
  return value.member;
}

template<typename T>
bool has_value(const T &, long)
{
  return false;
}

struct present
{
  bool member;
};

struct missing
{
};

int main()
{
  present yes = {true};
  missing no;
  return has_value(yes, 0) && !has_value(no, 0) ? 0 : 1;
}
