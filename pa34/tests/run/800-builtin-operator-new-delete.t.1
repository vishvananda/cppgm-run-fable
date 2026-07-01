namespace std {
inline namespace __1 {
enum class align_val_t : decltype(sizeof(0)) {};
}
using __1::align_val_t;
}

void* f() {
  void* p0 = __builtin_operator_new(sizeof(int));
  __builtin_operator_delete(p0);

  void* p1 = __builtin_operator_new(sizeof(int), static_cast<std::align_val_t>(16));
  __builtin_operator_delete(p1, static_cast<std::align_val_t>(16));

  void* p2 = __builtin_operator_new(sizeof(int));
  __builtin_operator_delete(p2, sizeof(int));

  void* p3 = __builtin_operator_new(sizeof(int), static_cast<std::align_val_t>(16));
  __builtin_operator_delete(p3, sizeof(int), static_cast<std::align_val_t>(16));

  return p0 == p1 ? p2 : p3;
}

int main() {
  return f() == 0;
}
