struct token
{
};

template<class Sig>
struct wrapper;

template<class R, class... Args>
struct wrapper<R(Args...)>
{
  int value;

  wrapper() : value(0)
  {
  }

  template<class T>
  wrapper& operator=(T)
  {
    value = 7;
    return *this;
  }
};

int main()
{
  wrapper<void()> *w = 0;
  *w = 0;
  return 0;
}
