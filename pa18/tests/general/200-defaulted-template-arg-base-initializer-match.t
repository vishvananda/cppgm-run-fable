template<class T>
struct Def {};

template<class T, class U = Def<T> >
struct Base {
  Base(int) {}
  Base(Base&&) {}
};

struct Derived : Base<char> {
  Derived() : Base<char>(0) {}
  Derived(Derived&& other) : Base<char>(static_cast<Base<char>&&>(other)) {}
};

int main() {
  Derived a;
  Derived b(static_cast<Derived&&>(a));
  (void)b;
  return 0;
}
