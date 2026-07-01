// VALIDATION: compile-pass
// N3485 focus: 14.8.2.5 [temp.deduct.type], 14.8 [temp.fct.spec]

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

struct X
{
};

struct Y : X
{
};

namespace lib
{
  template<class T>
  class wrapper
  {
    template<class U> friend class wrapper;

  public:
    explicit wrapper(T &) : value(3) {}

    template<class Y, class = typename enable_if<true>::type>
    wrapper(wrapper<Y> other) : value(other.value + 1) {}

    int value;
  };

  template<class T>
  wrapper<T> const make(T & value)
  {
    return wrapper<T>(value);
  }
}

int accept(lib::wrapper<X> value, Y *)
{
  return value.value;
}

int main()
{
  Y value;
  return accept(lib::make(value), &value) == 4 ? 0 : 1;
}
