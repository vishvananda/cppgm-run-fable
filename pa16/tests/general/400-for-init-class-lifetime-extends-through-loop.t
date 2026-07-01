int live = 0;
int destroyed = 0;

struct Guard {
  Guard() noexcept { live = 1; }
  ~Guard() noexcept { live = 0; destroyed = destroyed + 1; }
  operator bool() const noexcept { return live != 0; }
};

int main()
{
  int saw_live = 0;
  for(Guard current; current; live = 0) {
    saw_live = live;
  }
  return saw_live == 1 && live == 0 && destroyed == 1 ? 0 : 1;
}
