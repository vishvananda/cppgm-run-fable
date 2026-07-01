// VALIDATION: compile-pass
// N3485 focus: core-language reduction of std::function assignment surface

template<typename Signature>
struct callable_box;

template<typename Result, typename Arg>
struct callable_box<Result(Arg)>
{
  typedef Result (*thunk_type)(const void *, Arg);

  const void * object;
  thunk_type thunk;

  callable_box() : object(0), thunk(0) {}

  template<typename Functor>
  void assign(const Functor & functor)
  {
    object = &functor;
    thunk = &invoke<Functor>;
  }

  Result operator()(Arg arg) const
  {
    return thunk(object, arg);
  }

private:
  template<typename Functor>
  static Result invoke(const void * object, Arg arg)
  {
    return (*static_cast<const Functor *>(object))(arg);
  }
};

struct add_one
{
  int operator()(int value) const
  {
    return value + 1;
  }
};

int main()
{
  add_one functor;
  callable_box<int(int)> fn;
  fn.assign(functor);
  return fn(4) == 5 ? 0 : 1;
}
