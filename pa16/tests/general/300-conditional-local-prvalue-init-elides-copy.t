struct S {
  S() noexcept;
  S(const S&) noexcept;
  ~S() noexcept;
};

S::S() noexcept {}
S::S(const S&) noexcept {}
S::~S() noexcept {}

S f() noexcept { return S(); }
S g() noexcept { return S(); }

int main() {
  bool c = true;
  S s = c ? f() : g();
  return 0;
}
