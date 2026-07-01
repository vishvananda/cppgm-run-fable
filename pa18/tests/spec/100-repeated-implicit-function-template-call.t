// VALIDATION: compile-pass
// N3485 focus: 14.7.1 [temp.inst], 14.8 [temp.fct.spec]

struct token {};

template<class T>
int f(const T &)
{
  return 1;
}

int main()
{
  token t;
  return f(t) + f(t) == 2 ? 0 : 1;
}
