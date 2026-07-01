template <class T>
T&& declval();

template <bool B, class T = void>
struct enable_if {};

template <class T>
struct enable_if<true, T> {
  using type = T;
};

template <bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template <class A, class B>
struct is_same {
  static const bool value = false;
};

template <class A>
struct is_same<A, A> {
  static const bool value = true;
};

template <class T>
struct remove_ref {
  using type = T;
};

template <class T>
struct remove_ref<T&> {
  using type = T;
};

template <class T>
struct remove_ref<T&&> {
  using type = T;
};

template <class T>
using remove_ref_t = typename remove_ref<T>::type;

template <class T>
T&& forward(remove_ref_t<T>& t) {
  return static_cast<T&&>(t);
}

template <int I>
struct priority_tag : priority_tag<I - 1> {};

template <>
struct priority_tag<0> {};

namespace api {
inline namespace v1 {

template <class _KeyT, class _Ret, class _WithKey, class _WithoutKey, class... _Args>
_Ret __try_key_extraction_impl(priority_tag<0>, _WithKey, _WithoutKey __without_key, _Args&&... __args) {
  return __without_key(forward<_Args>(__args)...);
}

template <class _KeyT,
          class _Ret,
          class _WithKey,
          class _WithoutKey,
          class _Arg1,
          class _Arg2,
          enable_if_t<is_same<_KeyT, remove_ref_t<_Arg1> >::value, int> = 0>
_Ret __try_key_extraction_impl(priority_tag<1>, _WithKey __with_key, _WithoutKey, _Arg1&& __arg1, _Arg2&& __arg2) {
  return __with_key(__arg1, forward<_Arg1>(__arg1), forward<_Arg2>(__arg2));
}

template <class _KeyT, class _WithKey, class _WithoutKey, class... _Args>
decltype(declval<_WithoutKey>()(declval<_Args>()...))
__try_key_extraction(_WithKey __with_key, _WithoutKey __without_key, _Args&&... __args) {
  using _Ret = decltype(__without_key(forward<_Args>(__args)...));
  return api::__try_key_extraction_impl<_KeyT, _Ret>(
      priority_tag<1>(), __with_key, __without_key, forward<_Args>(__args)...);
}

}
}

struct WithKey {
  int operator()(const int&, int&, unsigned long&) const {
    return 1;
  }
};

struct WithoutKey {
  int operator()(int&, unsigned long&) const {
    return 1;
  }
};

int main() {
  int value = 0;
  unsigned long mapped = 1;
  return api::__try_key_extraction<int>(WithKey(), WithoutKey(), value, mapped) - 1;
}
