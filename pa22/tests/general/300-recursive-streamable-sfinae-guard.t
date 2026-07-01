template<bool, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class...>
struct make_void {
  typedef void type;
};

template<class... Ts>
using void_t = typename make_void<Ts...>::type;

template<class T>
T&& declval();

struct Sink {};
struct Value {};

Sink& operator>>(Sink&, Value&);

template<class Stream, class T, class = void>
struct is_streamable {
  static const bool value = false;
};

template<class Stream, class T>
struct is_streamable<Stream, T, void_t<decltype(declval<Stream>() >> declval<T>())> > {
  static const bool value = true;
};

template<class Stream, class T, enable_if_t<is_streamable<Stream, T>::value, int> = 0>
Stream&& operator>>(Stream&&, T&&);

static_assert(is_streamable<Sink&, Value&>::value, "");

int main() {
  return 0;
}
