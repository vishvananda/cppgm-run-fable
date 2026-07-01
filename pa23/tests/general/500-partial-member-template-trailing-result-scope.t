// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.2.5 [temp.deduct.type]
// A member template of a selected class partial specialization must reparse
// its trailing result type with the partial-specialization parameters in scope.

namespace detail {

template<class T>
T &&declval();

template<class T>
struct decay {
  typedef T type;
};

template<class T>
using decay_t = typename decay<T>::type;

template<class T>
struct type_identity {
  typedef T type;
};

template<class T>
using type_identity_t = typename type_identity<T>::type;

template<class T>
struct binder {
  T value;
};

template<class Token, class Signature>
struct async_result;

template<class CompletionToken, class Signature, class Initiation, class... Args>
auto async_initiate(Initiation &&initiation,
                    type_identity_t<CompletionToken> &token,
                    Args &&... args)
  -> decltype(async_result<decay_t<CompletionToken>, Signature>::initiate(
      static_cast<Initiation&&>(initiation),
      static_cast<CompletionToken&&>(token),
      static_cast<Args&&>(args)...));

template<class T, class Signature>
struct async_result {
  template<class Initiation, class RawCompletionToken, class... Args>
  static int initiate(Initiation &&, RawCompletionToken &&, Args &&...)
  {
    return 0;
  }
};

template<class T, class Signature>
struct async_result<binder<T>, Signature> {
  template<class Initiation>
  struct init_wrapper {
  };

  template<class Initiation, class RawCompletionToken, class... Args>
  static auto initiate(Initiation &&, RawCompletionToken &&token, Args &&... args)
    -> decltype(async_initiate<T, Signature>(
        declval<init_wrapper<decay_t<Initiation> > >(),
        token.value,
        static_cast<Args&&>(args)...));
};

struct handler {
};

struct initiation {
};

}  // namespace detail

int main()
{
  detail::binder<detail::handler> b;
  detail::async_initiate<detail::binder<detail::handler>, void(int)>(
      detail::initiation(), b, 1);
  return 0;
}
