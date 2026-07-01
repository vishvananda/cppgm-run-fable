// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call]

template<class T>
struct ref_view
{
  T * ptr;

  explicit ref_view(T & value)
    : ptr(&value)
  {
  }

  T & get() const
  {
    return *ptr;
  }
};

template<class T>
ref_view<T const> cref_like(T const & value)
{
  return ref_view<T const>(value);
}

int main()
{
  int const volatile value = 4;
  return &cref_like(value).get() == &value ? 0 : 1;
}
