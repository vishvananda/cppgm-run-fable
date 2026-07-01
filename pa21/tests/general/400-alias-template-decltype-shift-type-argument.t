template<class T> struct id { typedef T type; };
template<class T> using id_t = typename id<T>::type;

int main() {
  id_t<decltype((8) >> (1))> x = 4;
  return x - 4;
}
