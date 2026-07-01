// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call], 5.3.1 [expr.unary.op]

struct Stream {};

struct Logger
{
  void set_stream(Stream &) {}
  void set_stream(int, Stream &) {}
};

Logger unit_test_log;
Stream cout_stream;

namespace boost
{
  template<class T>
  struct reference_wrapper
  {
    T * value;
    reference_wrapper(T & t) : value(&t) {}
  };

  template<class T>
  reference_wrapper<T> ref(T & t)
  {
    return reference_wrapper<T>(t);
  }

  template<class R, class T, class B1, class A1, class A2>
  int bind(R (T::* f)(B1), A1 object, A2 bound)
  {
    return (object && f && bound.value) ? 7 : 0;
  }
}

int main()
{
  return boost::bind(&Logger::set_stream,
                     &unit_test_log,
                     boost::ref(cout_stream)) == 7 ? 0 : 1;
}
