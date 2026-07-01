template<class T> struct id { typedef T type; };
template<class T> using id_t = typename id<T>::type;

int main() {
  id_t<decltype((1) > (0))> x = true;
  return x ? 0 : 1;
}
