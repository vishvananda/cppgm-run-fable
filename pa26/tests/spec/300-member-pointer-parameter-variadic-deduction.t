// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call]

struct Stream {};

struct Logger
{
  void set_stream(Stream &) {}
};

template<class R, class T, class... A>
int mem_fn(R (T::* pmf)(A...))
{
  return pmf ? 7 : 0;
}

template<class R, class T, class B1>
int bind(R (T::* f)(B1))
{
  return mem_fn(f);
}

int main()
{
  return bind(&Logger::set_stream) == 7 ? 0 : 1;
}
