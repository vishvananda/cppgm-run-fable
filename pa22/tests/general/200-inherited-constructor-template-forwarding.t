// VALIDATION: compile-pass
// A dependent-base inherited constructor template participates in construction.

struct arg
{
  int value;

  explicit arg(int v)
    : value(v)
  {
  }
};

template <typename T>
struct base
{
  int value;

  template <typename U>
  explicit base(U&& u)
    : value(static_cast<U&&>(u).value)
  {
  }
};

template <typename T>
struct derived : base<T>
{
  using base<T>::base;
};

int main()
{
  arg a(7);
  derived<int> d(static_cast<arg&&>(a));
  return d.value == 7 ? 0 : 1;
}
