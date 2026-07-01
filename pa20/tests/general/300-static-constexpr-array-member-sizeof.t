// VALIDATION: compile-pass

struct base {
  static constexpr unsigned values[] = {1, 2, 3};
};

struct derived : base {
  unsigned f() { return sizeof(values) / sizeof(unsigned); }
};

int main() {
  derived d;
  return d.f() == 3 ? 0 : 1;
}
