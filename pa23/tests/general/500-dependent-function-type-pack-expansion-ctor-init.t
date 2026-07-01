template<class T>
struct decay {
  typedef T type;
};

template<class T>
using decay_t = typename decay<T>::type;

template<class... T>
struct tuple {
};

struct allocator {
};

template<class Allocator>
struct use_future_t {
};

template<class Signature, class Allocator>
struct promise_handler {
};

template<class Signature, class Allocator>
struct promise_async_result {
  typedef promise_handler<Signature, Allocator> completion_handler_type;

  explicit promise_async_result(completion_handler_type &) {
  }
};

template<class Token, class Signature>
struct async_result;

template<class Allocator, class Result, class... Args>
struct async_result<use_future_t<Allocator>, Result(Args...)>
  : promise_async_result<void(decay_t<Args>...), Allocator> {
  explicit async_result(
      typename promise_async_result<void(decay_t<Args>...),
                                    Allocator>::completion_handler_type & h)
    : promise_async_result<void(decay_t<Args>...), Allocator>(h) {
  }
};

struct error_code {
};

int main()
{
  typedef async_result<use_future_t<allocator>, void(tuple<error_code>)> result;
  promise_handler<void(tuple<error_code>), allocator> handler;
  result r(handler);
  (void)r;
  return 0;
}
