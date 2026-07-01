template<class T>
T declval();

template<class CompletionToken, class... Signatures>
struct async_result;

template<class CompletionToken, class... Signatures, class Initiation, class... Args>
auto async_initiate(Initiation && initiation, CompletionToken & token, Args &&... args)
  -> decltype(async_result<CompletionToken, Signatures...>::initiate(
      static_cast<Initiation &&>(initiation),
      token,
      static_cast<Args &&>(args)...));

struct handler {
};

template<class... Signatures>
struct async_result<handler, Signatures...> {
  template<class Initiation, class RawCompletionToken, class... Args>
  static int initiate(Initiation &&, RawCompletionToken &&, Args &&...);
};

template<class Token>
struct wrapped_token {
  Token token;
};

template<class Token>
wrapped_token<Token> wrap(Token token) {
  return wrapped_token<Token>{token};
}

template<class CompletionToken, class... Signatures>
struct async_result<wrapped_token<CompletionToken>, Signatures...>
  : async_result<CompletionToken, Signatures...> {
  template<class Initiation, class RawCompletionToken, class... Args>
  static auto initiate(Initiation && initiation,
                       RawCompletionToken && token,
                       Args &&... args)
    -> decltype(async_initiate<CompletionToken, Signatures...>(
        static_cast<Initiation &&>(initiation),
        token.token,
        static_cast<Args &&>(args)...));
};

struct initiation {
};

template<class WaitToken>
auto async_wait(WaitToken && token)
  -> decltype(async_initiate<WaitToken, void>(
      declval<initiation>(),
      token,
      7));

int main() {
  return sizeof(async_wait(wrap(wrap(handler())))) == sizeof(int) ? 0 : 1;
}
