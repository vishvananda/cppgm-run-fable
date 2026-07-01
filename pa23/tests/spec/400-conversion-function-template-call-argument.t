// VALIDATION: compile-pass
// N3485 focus: 12.3.2 [class.conv.fct], 14.8 [temp.fct.spec]

struct box
{
  int value;

  template<class T>
  operator T() const
  {
    return value;
  }
};

int f(int x)
{
  return x;
}

int main()
{
  box x = {7};
  return f(x) - 7;
}
