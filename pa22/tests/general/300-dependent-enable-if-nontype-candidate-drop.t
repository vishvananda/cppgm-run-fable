template<bool, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
const bool enabled = false;

int pick(...) {
  return 1;
}

template<class T, enable_if_t<enabled<T>, int> = 0>
int pick(T) {
  return 2;
}

int main() {
  return pick(0);
}
