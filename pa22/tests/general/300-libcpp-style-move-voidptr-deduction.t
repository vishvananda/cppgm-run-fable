// VALIDATION: compile-pass

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
struct remove_reference
{
  typedef T type;
};

template<class T>
struct remove_reference<T &>
{
  typedef T type;
};

template<class T>
struct remove_reference<T &&>
{
  typedef T type;
};

template<class T>
using remove_reference_t = typename remove_reference<T>::type;

template<class T>
remove_reference_t<T> && move_value(T && value)
{
  using U = remove_reference_t<T>;
  return static_cast<U &&>(value);
}

template<class T>
using swap_result_t = enable_if_t<true, void>;

template<class T>
swap_result_t<T> swap_values(T & x, T & y)
{
  T t(move_value(x));
  x = move_value(y);
  y = move_value(t);
}

void f(void *& x, void *& y)
{
  swap_values(x, y);
}

int main()
{
  return 0;
}
