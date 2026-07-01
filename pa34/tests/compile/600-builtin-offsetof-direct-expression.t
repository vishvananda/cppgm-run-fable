#if !__has_builtin(__builtin_offsetof)
#error expected __builtin_offsetof
#endif

struct object {
  char c;
  int i;
};

int direct_offset() {
  return (int)__builtin_offsetof(object, i);
}

static_assert(__builtin_offsetof(object, i) == 4, "");

int main() {
  return direct_offset() == 4 ? 0 : 1;
}
