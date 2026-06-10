// course/pa10: every lambda-introducer capture form in the shared
// grammar: capture-defaults & =, captures id &id this, pack
// expansions, and capture-defaults followed by capture-lists.
struct W {
  int m;
  int get() {
    auto a = [this]() { return m; };
    return a();
  }
};
template <class... Args>
int pack(Args... args) {
  auto p = [args...] { return 0; };
  auto q = [&, args...] { return 1; };
  return p() + q();
}
int main() {
  int x = 1, y = 2;
  auto e = [] { return 0; };
  auto rd = [&] { return x; };
  auto cd = [=] { return y; };
  auto rx = [&x] { return x; };
  auto cx = [x] { return x; };
  auto mixed = [x, &y] { return x + y; };
  auto dmix = [=, &x] { return x + y; };
  auto rmix = [&, y] { return x + y; };
  return e() + rd() + cd() + rx() + cx() + mixed() + dmix() + rmix();
}
