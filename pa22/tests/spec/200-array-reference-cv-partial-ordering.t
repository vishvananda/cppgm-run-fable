// VALIDATION: compile-pass
// N3485 focus: 14.8.2.4 [temp.deduct.partial], 13.3.3 [over.match.best]

namespace ns
{
  typedef unsigned long size_t;

  int selected = 0;

  namespace detail
  {
    template<class C>
    const int * range_begin(C &)
    {
      selected = 1;
      return 0;
    }

    template<class T, size_t N>
    const int * range_begin(const T (&a)[N])
    {
      selected = 2;
      return a;
    }

    template<class T, size_t N>
    const int * range_begin(T (&a)[N])
    {
      selected = 3;
      return a;
    }
  }

  template<class T>
  const int * begin(const T & r)
  {
    using namespace detail;
    return range_begin(r);
  }
}

int main()
{
  int data[3] = {0, 0, 0};
  const int const_data[3] = {0, 0, 0};

  ns::detail::range_begin(data);
  if(ns::selected != 3) {
    return 1;
  }

  ns::begin(data);
  if(ns::selected != 2) {
    return 2;
  }

  ns::detail::range_begin(const_data);
  if(ns::selected != 2) {
    return 3;
  }

  return 0;
}
