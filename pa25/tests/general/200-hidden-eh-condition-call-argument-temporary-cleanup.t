// VALIDATION: compile-pass
// N3485 focus: 12.2 [class.temporary], 6.4 [stmt.select]

struct Probe
{
  static int destroyed;

  ~Probe() noexcept
  {
    ++destroyed;
  }
};

int Probe::destroyed = 0;

int truthy(const Probe &) noexcept
{
  return 1;
}

int main()
{
  if(truthy(Probe())) {
    return Probe::destroyed == 1 ? 0 : Probe::destroyed + 10;
  }
  return 20;
}
