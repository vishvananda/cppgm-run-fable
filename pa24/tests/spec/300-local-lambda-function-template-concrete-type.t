// VALIDATION: compile-pass
// N3485 focus: 14.6.1 [temp.local], 14.8.2 [temp.deduct]

template<class T>
struct box
{
  ~box() {}
};

template<class T>
int use(box<T> &)
{
  return 0;
}

int main()
{
  auto f = []()
  {
    struct local
    {
    };

    box<local> x;
    return use(x);
  };

  return f();
}
