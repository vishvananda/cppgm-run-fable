template<class... Args>
int invoke(Args&&...);

template<class F, class... Args>
auto invoke(F&& f, Args&&... args) -> decltype(f(args...)) {
  return f(args...);
}

int g() {
  return 7;
}

int main() {
  int (*fp)() = g;
  return invoke(fp) != 7;
}
