template<class T>
T&& move_like(T& t)
{
  return static_cast<T&&>(t);
}

struct Policy {};

template<class>
struct Ops;

template<>
struct Ops<Policy>
{
  template<class Iter>
  static Iter&& iter_move(Iter& i)
  {
    return move_like(i);
  }
};

template<class T>
void sift(T* p)
{
  typedef T value_type;
  value_type top(Ops<Policy>::iter_move(*p));
}

void f()
{
  struct Local {
    Local() {}
    Local(const Local&) {}
    Local(Local&&) {}
  };
  Local a[1];
  sift(a);
}

int main()
{
  return 0;
}
