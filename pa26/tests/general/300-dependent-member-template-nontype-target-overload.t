// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype], 14.8.2.2 [temp.deduct.funcaddr]

struct callable
{
  template<class U>
  void f(U) const {}
};

template<class T>
struct member
{
  typedef void (T::*mem_func_ptr)(long) const;

  template<mem_func_ptr MemFuncPtr>
  static void wrap(void *) {}

  template<mem_func_ptr MemFuncPtr, class T0>
  static void wrap(void *, T0) {}
};

typedef void (*entry)(void *, long);

template<class T>
struct holder
{
  static entry const value;
};

template<class T>
entry const holder<T>::value = &member<T>::template wrap<&T::f>;

int main()
{
  return holder<callable>::value ? 0 : 1;
}
