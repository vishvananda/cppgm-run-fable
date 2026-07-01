int destroyed = 0;

struct Box {
  ~Box() noexcept { destroyed = destroyed + 1; }
  int* get() const noexcept { return 0; }
};

int main()
{
  int flag = 0;
  int * q = flag ? Box().get() : (int*)0;
  return q == 0 && destroyed == 0 ? 0 : 1;
}
