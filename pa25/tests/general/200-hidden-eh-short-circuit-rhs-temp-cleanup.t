int constructed = 0;
int destroyed = 0;

struct Probe {
  ~Probe() noexcept { ++destroyed; }
};

int use(const Probe &) noexcept { ++constructed; return 1; }

int truthy(const char * value) noexcept
{
  return value && use(Probe());
}

int main()
{
  return (truthy((const char*)0) == 0 && constructed == 0 && destroyed == 0) ? 0 : 1;
}
