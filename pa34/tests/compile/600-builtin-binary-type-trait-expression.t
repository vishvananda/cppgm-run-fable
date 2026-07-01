static_assert(__is_same(int, int), "");
static_assert(__is_assignable(int&, int), "");
static_assert(__is_convertible(int, long), "");

int main() {
  return 0;
}
