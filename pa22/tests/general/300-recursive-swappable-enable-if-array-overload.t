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

template<class T>
void swap(T&, T&);

template<class T, class U, class = void>
const bool swappable_with_v = false;

template<class T>
const bool swappable_v = swappable_with_v<T&, T&>;

template<class T, unsigned N, enable_if_t<swappable_v<T>, int> = 0>
void swap(T (&)[N], T (&)[N]);

template<class T, class U>
const bool swappable_with_v<T, U,
    void_t<decltype(swap(declval<T>(), declval<U>())),
           decltype(swap(declval<U>(), declval<T>()))>> = true;

static_assert(swappable_with_v<int&, int&>, "");

int main() {
  return 0;
}
