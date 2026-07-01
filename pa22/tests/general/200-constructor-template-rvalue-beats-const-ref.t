// VALIDATION: compile-pass
// Rvalue arguments should select the forwarding constructor template.

template <class T>
struct remove_reference
{
  typedef T type;
};

template <class T>
struct remove_reference<T &>
{
  typedef T type;
};

template <class T>
struct remove_reference<T &&>
{
  typedef T type;
};

template <class T>
T && forward(typename remove_reference<T>::type & value)
{
  return static_cast<T &&>(value);
}

struct movable
{
  int value;

  explicit movable(int v)
    : value(v)
  {
  }

  movable(const movable &) = delete;

  movable(movable && other)
    : value(other.value)
  {
    other.value = -1;
  }
};

template <class A, class B>
struct pair
{
  A first;
  B second;

  pair(const A & a, const B & b)
    : first(a), second(b)
  {
  }

  template <class U, class V>
  pair(U && u, V && v)
    : first(forward<U>(u)), second(forward<V>(v))
  {
  }
};

template <class T, class U, class V>
T build(U && u, V && v)
{
  return T(forward<U>(u), forward<V>(v));
}

int main()
{
  movable a(1);
  movable b(2);
  pair<movable, movable> p =
      build<pair<movable, movable> >(static_cast<movable &&>(a),
                                     static_cast<movable &&>(b));
  return p.first.value == 1 && p.second.value == 2 ? 0 : 1;
}
