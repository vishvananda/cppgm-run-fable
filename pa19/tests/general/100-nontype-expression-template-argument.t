typedef unsigned long uintptr_t;

template<class T, T v>
struct integral_constant {};

struct X {
  typedef integral_constant<uintptr_t, (1ULL << ((8 * sizeof(uintptr_t)) - 1))> B;
};

int main() {
  return 0;
}
