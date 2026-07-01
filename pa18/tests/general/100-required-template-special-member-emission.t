int total;

template<typename T>
struct box {
  T value;

  box() noexcept : value(1) {}
  box(const box & other) noexcept : value(other.value + 2) {}
  ~box() noexcept { total = total + value; }
};

int main()
{
  {
    box<int> a;
    box<int> b(a);
    total = total + b.value;
  }
  return total - 7;
}
