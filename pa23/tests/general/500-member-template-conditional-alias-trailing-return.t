// VALIDATION: compile-pass
// An out-of-class member definition can cause a dependent source-owner class
// template to collect member templates before a concrete instantiation uses
// them. A member template trailing return still needs the concrete owner
// parameter bindings when it selects Stream through conditional_t before
// resolving a forwarded member call.

template<class T>
T&& declval();

template<class T>
struct remove_reference
{
  typedef T type;
};

template<class T>
struct remove_reference<T&>
{
  typedef T type;
};

template<class T>
using remove_reference_t = typename remove_reference<T>::type;

template<bool, class T, class>
struct conditional
{
  typedef T type;
};

template<class T, class U>
struct conditional<false, T, U>
{
  typedef U type;
};

template<bool B, class T, class U>
using conditional_t = typename conditional<B, T, U>::type;

template<class Executor>
struct default_completion_token
{
};

template<class Token, class Signature>
struct async_result
{
  typedef int return_type;

  template<class Initiation, class RawToken>
  static int initiate(Initiation&&, RawToken&&)
  {
    return 9;
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

struct tcp
{
};

struct executor
{
};

template<class Protocol, class Executor>
struct basic_stream_socket
{
  typedef int executor_type;
  typedef basic_stream_socket lowest_layer_type;

  template<class Buffer, class Token>
  auto async_write_some(const Buffer& buffer, Token&& token)
      -> decltype(async_initiate<Token, void(int)>(
          declval<int>(), static_cast<Token&&>(token)))
  {
    return async_initiate<Token, void(int)>(
        declval<int>(), static_cast<Token&&>(token));
  }
};

template<class Stream>
struct wrapper
{
  typedef remove_reference_t<Stream> next_layer_type;
  typedef typename next_layer_type::lowest_layer_type lowest_layer_type;
  typedef typename lowest_layer_type::executor_type executor_type;

  static const int npos;

  template<class Buffer,
           class Handler = default_completion_token<executor_type> >
  auto async_write_some(const Buffer& buffer,
      Handler&& handler = default_completion_token<executor_type>())
      -> decltype(
        declval<conditional_t<true, Stream&, Handler> >().async_write_some(
          buffer, static_cast<Handler&&>(handler)))
  {
    return next_layer_.async_write_some(buffer,
        static_cast<Handler&&>(handler));
  }

  Stream next_layer_;
};

template<class Stream>
const int wrapper<Stream>::npos = 0;

void handler(int)
{
}

int main()
{
  wrapper<basic_stream_socket<tcp, executor> > w;
  return w.async_write_some(1, &handler) == 9 ? 0 : 1;
}
