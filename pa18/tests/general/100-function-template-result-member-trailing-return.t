// VALIDATION: compile-pass

struct handler
{
  int value;

  int get()
  {
    return value;
  }
};

template<class T>
struct result
{
  T target_;

  auto get() -> decltype(target_.get())
  {
    return target_.get();
  }
};

template<class Token>
auto test(Token& token) -> decltype(result<Token>().get())
{
  result<Token> r;
  r.target_ = token;
  return r.get();
}

int main()
{
  handler h;
  h.value = 7;
  return test(h) == 7 ? 0 : 1;
}
