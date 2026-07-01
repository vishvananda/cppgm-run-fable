// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype]

int square(int x)
{
  return x * x;
}

template<int (*Fn)(int)>
struct evaluator
{
  static int run(int x)
  {
    return Fn(x);
  }
};

int main()
{
  return evaluator<square>::run(5) == 25 ? 0 : 1;
}
