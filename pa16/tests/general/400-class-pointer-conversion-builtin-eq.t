// VALIDATION: compile-pass

struct Y
{
  operator char const*() const
  {
    return "Y";
  }
};

int main()
{
  Y a;
  Y b;
  return a == b ? 0 : 1;
}
