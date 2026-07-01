struct Counter {
  int *p;
  ~Counter() noexcept { ++*p; }
};

struct Hooks {
  Counter c;
};

Hooks make_hooks(int *p) noexcept
{
  Hooks hooks = {{p}};
  return hooks;
}
int read(const Hooks &h) noexcept { return *h.c.p; }

int main() {
  int destroyed = 0;
  int before = read(make_hooks(&destroyed));
  int after = destroyed;
  return (before == 0 && after == 1) ? 0 : 1;
}
