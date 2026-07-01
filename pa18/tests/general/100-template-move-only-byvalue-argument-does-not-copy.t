struct move_only {
  explicit move_only(int *value) : value_(value) {}
  move_only(move_only &&other) : value_(other.value_) { other.value_ = 0; }

private:
  move_only(const move_only &);

public:
  int *value_;
};

template<class T>
struct box {
  explicit box(T &&value) : value_(static_cast<T &&>(value)) {}
  box(box &&other) : value_(static_cast<T &&>(other.value_)) {}
  box(const box &other) : value_(other.value_) {}
  T value_;
};

int sink(box<move_only> value, int *expected)
{
  return value.value_.value_ == expected ? 0 : 1;
}

int main()
{
  int value = 3;
  move_only source(&value);
  box<move_only> boxed(static_cast<move_only &&>(source));
  return sink(static_cast<box<move_only> &&>(boxed), &value);
}

// VALIDATION: compile-pass
