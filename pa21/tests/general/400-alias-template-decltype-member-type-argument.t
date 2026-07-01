template<class T> struct id { typedef T type; };
template<class T> using id_t = typename id<T>::type;

struct A { int a_value; };

int main() {
  id_t<decltype(((A*)0)->a_value)> x = 0;
  return x;
}
