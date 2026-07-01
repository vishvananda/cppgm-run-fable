// VALIDATION: compile-pass
// N3485 focus: 14.8.1 [temp.arg.explicit], 14.8.2 [temp.deduct]
// A function-template overload with too few non-pack template parameters is
// not viable for an explicit template-id with more arguments.

struct one {};
struct three {};

template<class T>
struct holder
{
  template<class A>
  one pick(int)
  {
    return one();
  }

  template<class A, class B, class C>
  three pick(int)
  {
    return three();
  }
};

int score(one)
{
  return 1;
}

int score(three)
{
  return 3;
}

int main()
{
  holder<int> h;
  return score(h.template pick<int, int, int>(0)) == 3 ? 0 : 1;
}
