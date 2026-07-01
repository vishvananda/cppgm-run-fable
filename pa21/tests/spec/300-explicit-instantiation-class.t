// VALIDATION: compile-pass
// N3485 focus: 14.7.2 [temp.explicit]

template<typename T>
struct holder
{
  T value;

  explicit holder(T v) : value(v) {}

  T get() const
  {
    return value;
  }
};

extern template struct holder<int>;
template struct holder<int>;

int main()
{
  holder<int> h(7);
  return h.get() == 7 ? 0 : 1;
}
