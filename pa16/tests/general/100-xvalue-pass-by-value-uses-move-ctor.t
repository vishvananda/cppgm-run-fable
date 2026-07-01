struct S {
  S() : tag(0) {}
  S(S &&) : tag(1) {}
  S(const S &) : tag(2) {}
  int tag;
};

int sink(S value)
{
  return value.tag;
}

int main()
{
  S source;
  return sink(static_cast<S &&>(source)) == 1 ? 0 : 1;
}

// VALIDATION: compile-pass
