// VALIDATION: compile-pass
// N3485 focus: 12.3.2 [class.conv.fct], 14.8 [temp.fct.spec]

template<typename Tag>
struct from
{
  template<typename U>
  operator U() const
  {
    return U(6);
  }
};

int consume_int(int value)
{
  return value;
}

int main()
{
  from<int> value;
  return consume_int(value) == 6 ? 0 : 1;
}
