template<class... Args>
struct Box {
  static int f(int prefix, Args... args);
};

template<class... Args>
int Box<Args...>::f(int prefix, Args...) {
  return prefix + sizeof...(Args);
}

int main() {
  return Box<>::f(7) == 7 && Box<int, float>::f(5, 1, 2.0f) == 7 ? 0 : 1;
}
