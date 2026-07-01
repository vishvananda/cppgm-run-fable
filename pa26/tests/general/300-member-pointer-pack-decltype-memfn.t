// A member-function pointer overload can carry a type pack inside the pointed-to
// function parameter list. A trailing decltype that wraps that pointer with a
// mem_fn-style helper must instantiate the bound Args... parameters before the
// nested call is checked.

namespace model {

struct unspecified {};

template<class PMF, class R, class T, class... Args>
struct function_wrapper {
  PMF pmf;
};

template<class R, class T, class... Args>
function_wrapper<decltype(static_cast<R (T::*)(Args...)>(0)), R, T, Args...>
mem_fn(R (T::*pmf)(Args...))
{
  function_wrapper<decltype(pmf), R, T, Args...> out = { pmf };
  return out;
}

template<class... Args>
struct list_av {
  typedef int type;
};

template<class R, class F, class L>
struct bind_t {
  F f;
};

template<class F, class... Args>
bind_t<unspecified, F, typename list_av<Args...>::type>
bind(F f, Args...)
{
  bind_t<unspecified, F, typename list_av<Args...>::type> out = { f };
  return out;
}

template<class R, class T, class B1, class B2,
         class A1, class A2, class A3>
auto bind(R (T::*f)(B1, B2), A1 a1, A2 a2, A3 a3)
  -> decltype(model::bind(model::mem_fn(f), a1, a2, a3))
{
  return model::bind(model::mem_fn(f), a1, a2, a3);
}

}

struct stream {};

struct log_t {
  void set_stream(int, stream &) {}
  void set_stream(stream &) {}
};

int test(log_t *log, stream &out)
{
  (void)model::bind(&log_t::set_stream, log, 1, out);
  return 0;
}

int main()
{
  log_t log;
  stream out;
  return test(&log, out);
}
