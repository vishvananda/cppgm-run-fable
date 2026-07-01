struct executor
{
  executor() : value(11) {}
  executor(const executor & other) : value(other.value + 1) {}
  ~executor() {}

  int value;
};

struct source
{
  executor value;

  executor get_executor() const
  {
    return value;
  }
};

struct binder_base
{
  binder_base(source & other)
    : executor_(static_cast<executor &&>(other.get_executor()))
  {
  }

  executor executor_;
};

int main()
{
  source s;
  binder_base b(s);
  return b.executor_.value == 13 ? 0 : 1;
}
// VALIDATION: compile-pass
