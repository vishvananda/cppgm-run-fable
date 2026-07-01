struct Parsed {
  int value;
};

template<class Fn, class ContextFn>
auto wrap_as_substitution(Fn fn, ContextFn context_fn) -> decltype(fn())
{
  try {
    return fn();
  } catch(int) {
    return context_fn();
  }
}

template<class T>
Parsed instantiate_value(T value)
{
  return wrap_as_substitution(
      [&]() -> Parsed
      {
        return Parsed{static_cast<int>(value)};
      },
      [&]() -> Parsed
      {
        return Parsed{-1};
      });
}

int main()
{
  Parsed parsed = instantiate_value(7);
  return parsed.value == 7 ? 0 : 1;
}
