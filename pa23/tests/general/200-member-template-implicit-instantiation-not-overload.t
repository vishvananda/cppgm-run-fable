// VALIDATION: compile-pass
// A cached implicit specialization of a member function template is not a
// separate overload candidate. The second call must deduce Handler from the
// function lvalue instead of also considering the earlier pointer specialization.

template<class T>
T&& declval();

template<class Token, class Signature>
struct async_result
{
  template<class Initiation, class RawToken>
  static int initiate(Initiation&&, RawToken&&)
  {
    return 7;
  }
};

template<class Token, class Signature, class Initiation>
auto async_initiate(Initiation&& initiation, Token&& token)
    -> decltype(async_result<Token, Signature>::initiate(
        static_cast<Initiation&&>(initiation), static_cast<Token&&>(token)))
{
  return async_result<Token, Signature>::initiate(
      static_cast<Initiation&&>(initiation), static_cast<Token&&>(token));
}

struct stream
{
  template<class Buffer, class Handler>
  auto async_read_some(const Buffer& buffer, Handler&& handler)
      -> decltype(async_initiate<Handler, void(int)>(
          declval<int>(), static_cast<Handler&&>(handler)))
  {
    return async_initiate<Handler, void(int)>(
        declval<int>(), static_cast<Handler&&>(handler));
  }
};

void read_handler(int)
{
}

int main()
{
  stream s;
  s.async_read_some(1, &read_handler);
  return s.async_read_some(1, read_handler) == 7 ? 0 : 1;
}
