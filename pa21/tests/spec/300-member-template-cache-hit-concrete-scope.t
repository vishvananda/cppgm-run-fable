// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.7.1 [temp.inst]

template<class A>
struct traits
{
  typedef A type;
};

template<class A>
struct guard
{
  explicit guard(A) {}

  A * get()
  {
    return 0;
  }
};

template<class T>
struct base
{
  typedef typename traits<T>::type value_type;

  template<class U>
  value_type * create(U)
  {
    guard<value_type> g((value_type()));
    return g.get();
  }
};

template<class T>
struct holder : base<T>
{
  typedef typename base<T>::value_type value_type;

  value_type * back();
  value_type * front();
};

template<class T>
typename holder<T>::value_type * holder<T>::back()
{
  return this->create(0);
}

template<class T>
typename holder<T>::value_type * holder<T>::front()
{
  return this->create(0);
}

int main()
{
  holder<int> h;
  h.back();
  h.front();
  return 0;
}
