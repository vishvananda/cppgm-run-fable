// VALIDATION: compile-pass
// N3485 focus: 12.2 [class.temporary], 12.6.2 [class.base.init]

struct Counter
{
  static int count;

  Counter() noexcept
  {
    ++count;
  }

  Counter(const Counter &) noexcept
  {
    ++count;
  }

  Counter & operator=(const Counter &) noexcept
  {
    return *this;
  }

  ~Counter() noexcept
  {
    --count;
  }
};

int Counter::count = 0;

struct Holder
{
  Counter a;
  Counter b;
  Counter c;
  Counter d;
  Counter e;

  Holder(int, const Counter & value) noexcept
      : a(value), b(value), c(value), d(value), e(value)
  {}
};

int main()
{
  {
    Holder holder(5, Counter());
    if(Counter::count != 5) {
      return Counter::count;
    }
  }
  return Counter::count;
}
