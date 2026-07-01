// VALIDATION: compile-pass
// N3485 focus: 12.2 [class.temporary], 5.15 [expr.log.or], 6.4 [stmt.select]

int destroyed = 0;

struct Probe
{
  ~Probe() noexcept { ++destroyed; }
};

int use(const Probe &) noexcept
{
  return 1;
}

int main()
{
  if(0 || use(Probe())) {
    return destroyed == 1 ? 0 : destroyed + 10;
  }
  return 20;
}
