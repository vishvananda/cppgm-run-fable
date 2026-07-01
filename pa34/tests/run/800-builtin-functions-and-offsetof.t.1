template<class T>
struct P {
  T value;
  char pad;
};

template<class T>
const unsigned long V = __builtin_offsetof(P<T>, pad);

constexpr bool constant_eval() {
  return __builtin_is_constant_evaluated();
}

template<class T>
T* addr(T& x) {
  return __builtin_addressof(x);
}

int cmp(const char* a, const char* b) {
  return __builtin_strcmp(a, b) + __builtin_memcmp(a, b, 0);
}

void* mv(void* d, const void* s, unsigned long n) {
  return __builtin_memmove(d, s, n);
}

float f() { return __builtin_inff(); }
double g() { return __builtin_inf(); }
long double h() { return __builtin_infl(); }

void impossible() {
  __builtin_unreachable();
}

void fence() {
  __c11_atomic_thread_fence(0);
  __c11_atomic_signal_fence(0);
}

int main() {
  int x = 0;
  return constant_eval() ? (int)V<int> : (addr(x) == &x ? 0 : 1);
}
