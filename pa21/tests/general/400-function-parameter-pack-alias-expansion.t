template<class T>
using Wrap = T;

template<class... Args>
struct Box {
  static int f(Wrap<Args>...);
};

template<class... Args>
int Box<Args...>::f(Wrap<Args>...) {
  return sizeof...(Args);
}

int main() {
  return Box<>::f() != 0 || Box<int, float>::f(1, 2.0f) != 2;
}
