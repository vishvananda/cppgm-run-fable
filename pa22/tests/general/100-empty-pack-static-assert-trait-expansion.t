template<class...> struct _And;
template<> struct _And<> { static const bool value = true; };
template<class B1> struct _And<B1> { static const bool value = B1::value; };

template<class T> struct __decay { typedef T type; };
template<class T> using __decay_t = typename __decay<T>::type;

template<class T, class U> struct is_constructible { static const bool value = true; };

template<class _Fp, class... _Args>
void f(_Fp&&, _Args&&...) {
  static_assert(_And<is_constructible<__decay_t<_Args>, _Args>...>::value, "");
}

int main() {
  int x = 0;
  f(x);
}
