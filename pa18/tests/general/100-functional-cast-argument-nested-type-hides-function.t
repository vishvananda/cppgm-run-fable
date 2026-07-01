// VALIDATION: compile-pass
// Boost.Container reduction: template argument deduction must analyze
// `shadow_maker(&value)` as construction of the nested type, not as a call to
// the hidden outer function with the same unqualified name.

void shadow_maker(int *);

template<class F>
void invoke(F f)
{
  f();
}

struct host
{
  struct shadow_maker
  {
    int * out;

    explicit shadow_maker(int * p)
      : out(p)
    {
    }

    void operator()()
    {
      *out = 17;
    }
  };

  static int run()
  {
    int value = 0;
    invoke(shadow_maker(&value));
    return value;
  }
};

int main()
{
  return host::run() == 17 ? 0 : 1;
}
