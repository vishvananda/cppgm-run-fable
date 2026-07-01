// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.3 [temp.over]
// Disabled async-initiate fallback overloads with dependent result types must
// remain substitution failures after template argument replacement.

template <typename T>
T&& declval();

template <bool B, typename T>
struct enable_if
{
};

template <typename T>
struct enable_if<true, T>
{
  typedef T type;
};

template <bool B, typename T>
using enable_if_t = typename enable_if<B, T>::type;

template <typename T>
struct type_identity
{
  typedef T type;
};

template <typename T>
using type_identity_t = typename type_identity<T>::type;

template <typename T>
struct is_signature
{
  static const bool value = false;
};

template <typename R, typename A>
struct is_signature<R(A)>
{
  static const bool value = true;
};

template <typename... Ts>
struct are_completion_signatures;

template <>
struct are_completion_signatures<>
{
  static const bool value = true;
};

template <typename T, typename... Ts>
struct are_completion_signatures<T, Ts...>
{
  static const bool value =
      is_signature<T>::value && are_completion_signatures<Ts...>::value;
};

template <typename Token, typename... Signatures>
struct has_initiate
{
  static const bool value = true;
};

template <typename Token, typename... Signatures>
struct async_result
{
  typedef int return_type;

  template <typename Initiation, typename RawToken>
  static int initiate(Initiation&&, RawToken&&)
  {
    return 7;
  }
};

template <typename CompletionToken,
          typename... Signatures,
          typename Initiation>
auto async_initiate(Initiation&& initiation,
                    type_identity_t<CompletionToken>& token)
    -> decltype(enable_if_t<
        enable_if_t<
            are_completion_signatures<Signatures...>::value,
            has_initiate<CompletionToken, Signatures...>>::value,
        async_result<CompletionToken, Signatures...>>::initiate(
            static_cast<Initiation&&>(initiation),
            static_cast<CompletionToken&&>(token)))
{
  return async_result<CompletionToken, Signatures...>::initiate(
      static_cast<Initiation&&>(initiation),
      static_cast<CompletionToken&&>(token));
}

template <typename... Signatures,
          typename CompletionToken,
          typename Initiation>
auto async_initiate(Initiation&& initiation,
                    CompletionToken&& token)
    -> decltype(enable_if_t<
        enable_if_t<
            are_completion_signatures<Signatures...>::value,
            has_initiate<CompletionToken, Signatures...>>::value,
        async_result<CompletionToken, Signatures...>>::initiate(
            static_cast<Initiation&&>(initiation),
            static_cast<CompletionToken&&>(token)))
{
  return async_result<CompletionToken, Signatures...>::initiate(
      static_cast<Initiation&&>(initiation),
      static_cast<CompletionToken&&>(token));
}

template <typename CompletionToken,
          typename... Signatures,
          typename Initiation>
typename enable_if_t<
    !enable_if_t<
        are_completion_signatures<Signatures...>::value,
        has_initiate<CompletionToken, Signatures...>>::value,
    async_result<CompletionToken, Signatures...>>::return_type
async_initiate(Initiation&&, type_identity_t<CompletionToken>&)
{
  return 0;
}

template <typename... Signatures,
          typename CompletionToken,
          typename Initiation>
typename enable_if_t<
    !enable_if_t<
        are_completion_signatures<Signatures...>::value,
        has_initiate<CompletionToken, Signatures...>>::value,
    async_result<CompletionToken, Signatures...>>::return_type
async_initiate(Initiation&&, CompletionToken&&)
{
  return 0;
}

template <typename Executor>
struct timer
{
  template <typename WaitToken>
  auto async_wait(WaitToken&& token)
      -> decltype(async_initiate<WaitToken, void(int)>(declval<int>(), token))
  {
    int initiation = 0;
    return async_initiate<WaitToken, void(int)>(
        static_cast<int&&>(initiation), token);
  }
};

int main()
{
  timer<int> t;
  return t.async_wait<int>(1) == 7 ? 0 : 1;
}
